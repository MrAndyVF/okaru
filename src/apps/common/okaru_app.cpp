#include "apps/common/okaru_app.h"

#include "app/deferred_call_poll_source.h"
#include "app/main_loop.h"
#include "app/timer_poll_source.h"
#include "config/config_poll_source.h"
#include "core/input/keybind_matcher.h"
#include "core/log.h"
#include "core/process/process.h"
#include "core/ui_phase.h"
#include "dbus/session_bus.h"
#include "dbus/session_bus_poll_source.h"
#include "i18n/i18n.h"
#include "i18n/i18n_service.h"
#include "render/scene/input_area.h"
#include "theme/apps_palette.h"
#include "ui/controls/input.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/key_repeat_poll_source.h"

#include <algorithm>
#include <cmath>
#include <csignal>
#include <linux/input-event-codes.h>

namespace okaru::apps {

  namespace {

    constexpr Logger kLog("app");

    std::atomic<bool>* g_stop = nullptr;

    void onSignal(int /*signal*/) {
      if (g_stop != nullptr) {
        *g_stop = true;
      }
    }

  } // namespace

  std::optional<std::string> WlClipboard::clipboardText() {
    process::RunOptions options;
    options.timeout = std::chrono::seconds(2);
    options.maxOutputBytes = 1U << 20U;
    const auto result = process::runSync({"wl-paste", "--no-newline", "--type", "text"}, options);
    if (!result) {
      return std::nullopt;
    }
    return result.out;
  }

  void WlClipboard::setClipboardText(std::string text) {
    if (!process::runAsync({"wl-copy", "--", text})) {
      kLog.warn("wl-copy failed; is wl-clipboard installed?");
    }
  }

  OkaruApp::OkaruApp(AppOptions options) : m_options(std::move(options)) {}

  OkaruApp::~OkaruApp() {
    m_sessionBusSource.reset();
    m_sessionBus.reset();
    Input::setTextClipboard(nullptr);
    m_input.setSceneRoot(nullptr);
    m_root.reset();
    m_selectPopup.reset();
    m_surface.reset();
  }

  float OkaruApp::scale() const { return std::max(0.1F, m_config.config().accessibility.uiScale); }

  void OkaruApp::applyConfig() {
    const auto& shell = m_config.config().shell;
    Style::setCornerRadiusScale(shell.cornerRadiusScale);
    Style::setButtonBordersEnabled(shell.buttonBorders);
    Style::setInputBordersEnabled(shell.inputBorders);
    Style::setCardBordersEnabled(shell.cardBorders);
    Style::setRtl(i18n::Service::instance().rtl());
    m_render.setTextFontFamily(shell.fontFamily);
    m_render.setTextFontWeight(shell.fontWeight);
    m_render.setTextBaseDirection(i18n::Service::instance().rtl());
  }

  void OkaruApp::applyPalette() {
    // Sin archivo (la shell nunca corrió) queda la paleta OKARU de fábrica.
    if (const auto shared = okaru::theme::readAppsPalette()) {
      setResolvedThemeLight(shared->light);
      setPalette(okaru::theme::mapGeneratedPaletteMode(shared->tokens));
    }
  }

  int OkaruApp::run() {
    g_stop = &m_stop;
    std::signal(SIGTERM, onSignal);
    std::signal(SIGINT, onSignal);

    i18n::Service::instance().init(m_config.config().shell.lang);
    if (!m_wayland.connect() || !m_wayland.hasXdgShell()) {
      kLog.error("cannot connect to the Wayland display");
      return 1;
    }
    m_glShared.initialize(m_wayland.display(), m_config.config().shell.sharedGlContext);
    m_render.initialize(m_glShared);
    applyConfig();
    applyPalette();
    Input::setTextClipboard(&m_clipboard);
    // Mismas teclas de validar/cancelar/navegar que la shell, según la config del usuario.
    for (const auto action :
         {KeybindAction::Validate, KeybindAction::Cancel, KeybindAction::Left, KeybindAction::Right, KeybindAction::Up,
          KeybindAction::Down, KeybindAction::TabNext, KeybindAction::TabPrevious, KeybindAction::Copy,
          KeybindAction::Save, KeybindAction::Delete}) {
      KeybindMatcher::setMatcher(action, [this, action](std::uint32_t sym, std::uint32_t modifiers) {
        return m_config.matchesKeybind(action, sym, modifiers);
      });
    }
    Input::setValidateKeyMatcher([this](std::uint32_t sym, std::uint32_t modifiers) {
      return m_config.matchesKeybind(KeybindAction::Validate, sym, modifiers);
    });

    m_config.addReloadCallback([this] {
      i18n::Service::instance().setLanguage(m_config.config().shell.lang);
      applyConfig();
      if (m_onReload) {
        m_onReload();
      }
      requestRebuild();
    });
    std::error_code ec;
    std::filesystem::create_directories(okaru::theme::appsPalettePath().parent_path(), ec);
    // La shell reemplaza el archivo con rename: se vigila la carpeta y se espera la escritura completa.
    m_fileWatcher.watch(
        okaru::theme::appsPalettePath(),
        [this] {
          applyPalette();
          if (m_onReload) {
            m_onReload();
          }
          requestRebuild();
        },
        FileWatcher::WatchTrigger::WriteCompleted
    );

    m_wayland.setPointerEventCallback([this](const PointerEvent& event) { onPointer(event); });
    m_wayland.setKeyboardEventCallback([this](const KeyboardEvent& event) { onKey(event); });

    const float s = scale();
    m_surface = std::make_unique<ToplevelSurface>(m_wayland);
    m_surface->setRenderContext(&m_render);
    m_surface->setAnimationManager(&m_animations);
    m_surface->setClosedCallback([this] { quit(); });
    m_surface->setConfigureCallback([this](std::uint32_t, std::uint32_t) { m_surface->requestLayout(); });
    m_surface->setPrepareFrameCallback([this](bool needsUpdate, bool needsLayout) {
      prepareFrame(needsUpdate, needsLayout);
    });
    m_surface->setUpdateCallback([] {});
    const ToplevelSurfaceConfig surfaceConfig{
        .width = static_cast<std::uint32_t>(std::lround(m_options.width * s)),
        .height = static_cast<std::uint32_t>(std::lround(m_options.height * s)),
        .minWidth = static_cast<std::uint32_t>(std::lround(m_options.minWidth * s)),
        .minHeight = static_cast<std::uint32_t>(std::lround(m_options.minHeight * s)),
        .title = i18n::tr(m_options.titleKey),
        .appId = m_options.appId,
    };
    if (!m_surface->initialize(nullptr, surfaceConfig)) {
      kLog.error("cannot create the window");
      return 1;
    }

    DeferredCallPollSource deferred;
    TimerPollSource timers;
    KeyRepeatPollSource keyRepeat(m_wayland);
    ConfigPollSource configSource(m_config);
    FileWatchPollSource fileWatch(m_fileWatcher);
    m_selectPopup = std::make_unique<SelectDropdownPopup>(m_wayland, m_render);
    m_selectPopup->setShadowConfig(m_config.config().shell.shadow);
    m_selectPopup->setParent(m_surface->xdgSurface(), m_surface->wlSurface(), nullptr);

    MainLoop loop(
        m_wayland, m_stop,
        [this] {
          m_selectPopup.reset();
          m_surface.reset();
        },
        [&] {
          std::vector<PollSource*> sources{&deferred, &timers, &keyRepeat, &configSource, &fileWatch};
          if (m_sessionBusSource != nullptr) {
            sources.push_back(m_sessionBusSource.get());
          }
          return sources;
        }
    );
    loop.run();
    return 0;
  }

  void OkaruApp::quit() { m_stop = true; }

  void OkaruApp::setFullscreen(bool fullscreen) {
    m_fullscreen = fullscreen;
    if (m_surface != nullptr) {
      m_surface->setFullscreen(fullscreen);
    }
  }

  SessionBus* OkaruApp::sessionBus() {
    if (m_sessionBus == nullptr) {
      try {
        m_sessionBus = std::make_unique<SessionBus>();
        m_sessionBusSource = std::make_unique<SessionBusPollSource>(*m_sessionBus);
      } catch (const std::exception& e) {
        kLog.warn("no session bus: {}", e.what());
        return nullptr;
      }
    }
    return m_sessionBus.get();
  }

  float OkaruApp::bufferScale() const { return m_surface != nullptr ? m_surface->effectiveBufferScale() : 1.0F; }

  Renderer* OkaruApp::renderer() { return m_surface != nullptr ? &m_surface->renderTarget().renderer() : nullptr; }

  void OkaruApp::requestRebuild() {
    m_rebuild = true;
    if (m_surface != nullptr) {
      m_surface->requestLayout();
    }
  }

  void OkaruApp::invalidate() {
    if (m_root == nullptr || m_surface == nullptr) {
      return;
    }
    if (m_root->layoutDirty()) {
      m_surface->requestLayout();
    } else if (m_root->paintDirty()) {
      m_surface->requestRedraw();
    }
  }

  void OkaruApp::focus(InputArea* area) {
    m_input.setFocus(area);
    invalidate();
  }

  void OkaruApp::prepareFrame(bool /*needsUpdate*/, bool needsLayout) {
    if (m_surface == nullptr || m_surface->width() == 0 || m_surface->height() == 0) {
      return;
    }
    m_render.makeCurrent(m_surface->renderTarget());
    Renderer& renderer = m_surface->renderTarget().renderer();
    const auto w = static_cast<float>(m_surface->width());
    const auto h = static_cast<float>(m_surface->height());

    // El estado de cada app vive fuera de los controles: un cambio de tamaño reconstruye la escena entera.
    if (m_rebuild || m_root == nullptr || m_root->width() != w || m_root->height() != h) {
      UiPhaseScope layoutPhase(UiPhase::Layout);
      m_rebuild = false;
      m_input.stashTabFocus();
      m_input.setSceneRoot(nullptr);
      m_dragNode = nullptr;
      if (m_selectPopup != nullptr) {
        m_selectPopup->closeSelectDropdown();
      }
      m_root = m_build ? m_build(w, h, scale()) : std::make_unique<Node>();
      m_root->setSize(w, h);
      m_root->setPopupContext(m_selectPopup.get());
      m_root->setAnimationManager(&m_animations);
      m_root->layout(renderer);
      m_input.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
        m_wayland.setCursorShape(serial, shape);
      });
      m_input.setSceneRoot(m_root.get());
      m_input.restoreStashedTabFocus();
      m_surface->setSceneRoot(m_root.get());
      if (m_afterBuild) {
        m_afterBuild();
      }
      return;
    }
    if (needsLayout || m_root->layoutDirty()) {
      UiPhaseScope layoutPhase(UiPhase::Layout);
      m_root->layout(renderer);
    }
  }

  bool OkaruApp::dragRegionContains(float x, float y) const {
    if (m_dragNode == nullptr) {
      return false;
    }
    float left = 0.0F;
    float top = 0.0F;
    float right = 0.0F;
    float bottom = 0.0F;
    Node::transformedBounds(m_dragNode, left, top, right, bottom);
    return x >= left && x < right && y >= top && y < bottom;
  }

  void OkaruApp::onPointer(const PointerEvent& event) {
    if (m_surface == nullptr) {
      return;
    }
    if (m_selectPopup != nullptr && m_selectPopup->isSelectDropdownOpen()) {
      if (m_selectPopup->onPointerEvent(event)) {
        return;
      }
      if (event.type == PointerEvent::Type::Button && event.pressed) {
        m_selectPopup->closeSelectDropdown();
        return;
      }
    }
    const bool onThis = event.surface != nullptr && event.surface == m_surface->wlSurface();
    const auto x = static_cast<float>(event.sx);
    const auto y = static_cast<float>(event.sy);
    switch (event.type) {
    case PointerEvent::Type::Enter:
      if (onThis) {
        m_pointerInside = true;
        m_input.pointerEnter(x, y, event.serial);
      }
      break;
    case PointerEvent::Type::Leave:
      if (onThis) {
        m_pointerInside = false;
        m_input.pointerLeave();
      }
      break;
    case PointerEvent::Type::Motion:
      if (m_pointerInside) {
        m_input.pointerMotion(x, y, m_wayland.lastInputSerial());
      }
      break;
    case PointerEvent::Type::Button:
      if (m_pointerInside) {
        m_input.pointerMotion(x, y, event.serial);
        if (event.pressed && event.button == BTN_LEFT && m_input.hoveredArea() == nullptr && dragRegionContains(x, y)) {
          m_surface->beginMove(event.serial);
          break;
        }
        m_input.pointerButton(x, y, event.button, event.pressed, event.serial, event.time, event.touch);
      }
      break;
    case PointerEvent::Type::Axis:
      if (m_pointerInside) {
        m_input.pointerAxis(
            x, y, event.axis, event.axisSource, event.axisValue, event.axisDiscrete, event.axisValue120, event.axisLines
        );
      }
      break;
    }
    invalidate();
  }

  void OkaruApp::onKey(const KeyboardEvent& event) {
    if (m_selectPopup != nullptr && m_selectPopup->isSelectDropdownOpen()) {
      m_selectPopup->onKeyboardEvent(event);
      return;
    }
    if (m_keyHandler && m_keyHandler(event)) {
      invalidate();
      return;
    }
    m_input.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
    invalidate();
  }

} // namespace okaru::apps
