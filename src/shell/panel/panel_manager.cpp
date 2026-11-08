#include "shell/panel/panel_manager.h"

#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/input/key_chord.h"
#include "core/input/keybind_matcher.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "ipc/ipc_arg_parse.h"
#include "ipc/ipc_service.h"
#include "render/render_context.h"
#include "render/scene/input_area.h"
#include "scripting/plugin_id.h"
#include "shell/bar/bar_corner_shape.h"
#include "shell/bar/bar_reserved_zone.h"
#include "shell/panel/panel.h"
#include "shell/panel/panel_surface_style.h"
#include "shell/screen_position.h"
#include "shell/surface/shadow.h"
#include "shell/tooltip/tooltip_manager.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/context_menu_popup.h"
#include "ui/controls/select_dropdown_popup.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"
#include "util/sys_utils.h"
#include "wayland/hyprland/focus_grab_service.h"
#include "wayland/layer_surface.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <wayland-client-protocol.h>

PanelManager* PanelManager::s_instance = nullptr;

namespace {

  constexpr Logger kLog("panel");
  constexpr auto kKeyboardRelaxDelay = std::chrono::milliseconds(100);
  constexpr auto kCaptureCloseTimeout = std::chrono::milliseconds(250);

  // Layer-shell keyboard interactivity for a panel surface: `initial` is applied at
  // map time, `relaxed` (when set) shortly after by m_keyboardRelaxTimer.
  struct PanelKeyboardPlan {
    LayerShellKeyboard initial = LayerShellKeyboard::None;
    std::optional<LayerShellKeyboard> relaxed;
  };

  // A panel asking for None never takes keyboard focus, so the app the user is
  // typing into keeps it. Everything else reproduces the per-path behavior the
  // compositor workarounds need: a focus grab maps Exclusive so the panel wins the
  // keyboard from the grab and hands it back once settled, and an attached panel
  // takes focus only after the bar-anchored map, which the reveal would otherwise
  // race. `mode` therefore only selects between OnDemand and Exclusive on the
  // detached path of a compositor without focus-grab support.
  PanelKeyboardPlan
  resolvePanelKeyboardPlan(LayerShellKeyboard mode, bool hasFocusGrab, bool grabWillActivate, bool attached) {
    if (mode == LayerShellKeyboard::None) {
      return {.initial = LayerShellKeyboard::None, .relaxed = std::nullopt};
    }
    if (hasFocusGrab) {
      return {
          .initial = LayerShellKeyboard::Exclusive,
          .relaxed = grabWillActivate ? std::optional{LayerShellKeyboard::OnDemand} : std::nullopt
      };
    }
    if (attached) {
      return {.initial = LayerShellKeyboard::None, .relaxed = LayerShellKeyboard::Exclusive};
    }
    return {.initial = mode, .relaxed = std::nullopt};
  }

  bool blurTraceEnabled() {
    static const bool enabled = SysUtils::isEnvFlagOn("OKARU_BLUR_TRACE");
    return enabled;
  }

  struct BarVisibleRect {
    std::int32_t left = 0;
    std::int32_t top = 0;
    std::int32_t right = 0;
    std::int32_t bottom = 0;
  };

  BarVisibleRect
  resolveBarVisibleRect(const BarConfig& barConfig, std::int32_t outputWidth, std::int32_t outputHeight) {
    const bool barIsBottom = barConfig.position == "bottom";
    const bool barIsLeft = barConfig.position == "left";
    const bool barIsRight = barConfig.position == "right";
    const bool barIsVertical = barIsLeft || barIsRight;
    const std::int32_t mEdge = std::max(0, barConfig.marginEdge);
    const std::int32_t mEnds = std::max(0, barConfig.marginEnds);
    const std::int32_t thickness = std::max(0, barConfig.thickness);

    const std::int32_t left =
        barIsRight ? std::max(0, outputWidth - mEdge - thickness) : (barIsVertical ? mEdge : mEnds);
    const std::int32_t top =
        barIsBottom ? std::max(0, outputHeight - mEdge - thickness) : (barIsVertical ? mEnds : mEdge);
    const std::int32_t right = barIsVertical ? left + thickness : std::max(left, outputWidth - mEnds);
    const std::int32_t bottom = barIsVertical ? std::max(top, outputHeight - mEnds) : top + thickness;

    return BarVisibleRect{
        .left = left,
        .top = top,
        .right = right,
        .bottom = bottom,
    };
  }

  std::int32_t anchoredSurfaceOrigin(
      std::uint32_t anchor, std::uint32_t startAnchor, std::uint32_t endAnchor, std::int32_t startMargin,
      std::int32_t endMargin, std::int32_t outputExtent, std::int32_t surfaceExtent
  ) {
    const bool anchoredStart = (anchor & startAnchor) != 0;
    const bool anchoredEnd = (anchor & endAnchor) != 0;
    if (anchoredStart != anchoredEnd) {
      return anchoredStart ? startMargin : outputExtent - surfaceExtent - endMargin;
    }
    return (outputExtent - surfaceExtent) / 2;
  }

  InputRect panelInputRectForSurface(
      std::uint32_t anchor, std::int32_t marginTop, std::int32_t marginRight, std::int32_t marginBottom,
      std::int32_t marginLeft, std::int32_t outputWidth, std::int32_t outputHeight, std::uint32_t surfaceWidth,
      std::uint32_t surfaceHeight, std::int32_t insetX, std::int32_t insetY, std::uint32_t panelWidth,
      std::uint32_t panelHeight
  ) {
    const auto resolvedSurfaceWidth = static_cast<std::int32_t>(surfaceWidth);
    const auto resolvedSurfaceHeight = static_cast<std::int32_t>(surfaceHeight);
    const auto surfaceX = anchoredSurfaceOrigin(
        anchor, LayerShellAnchor::Left, LayerShellAnchor::Right, marginLeft, marginRight, outputWidth,
        resolvedSurfaceWidth
    );
    const auto surfaceY = anchoredSurfaceOrigin(
        anchor, LayerShellAnchor::Top, LayerShellAnchor::Bottom, marginTop, marginBottom, outputHeight,
        resolvedSurfaceHeight
    );
    return InputRect{surfaceX + insetX, surfaceY + insetY, static_cast<int>(panelWidth), static_cast<int>(panelHeight)};
  }

  InputRect boundsForPanelTrace(const std::vector<InputRect>& rects) {
    if (rects.empty()) {
      return {};
    }

    int minX = rects.front().x;
    int minY = rects.front().y;
    int maxX = rects.front().x + rects.front().width;
    int maxY = rects.front().y + rects.front().height;
    for (const auto& rect : rects) {
      minX = std::min(minX, rect.x);
      minY = std::min(minY, rect.y);
      maxX = std::max(maxX, rect.x + rect.width);
      maxY = std::max(maxY, rect.y + rect.height);
    }

    return InputRect{minX, minY, maxX - minX, maxY - minY};
  }

  // Resolves the bar a panel should attach to / position relative to.
  // `shell.panel_anchor_bar` wins when set; otherwise `barName` is the opening
  // source bar. A named bar that does not exist fails loudly (nullopt).
  // Active bar instances are authoritative because temporary IPC state lives
  // there. Without an active instance, retain the configured bar so a panel can
  // use its floating or center-screen placement.
  std::optional<BarConfig> resolvePanelBarConfig(
      ConfigService* configService, CompositorPlatform* platform, wl_output* output,
      const std::function<std::optional<BarConfig>(wl_output*, std::string_view)>& barConfigProvider,
      std::string_view barName = {}
  ) {
    if (configService == nullptr || configService->config().bars.empty()) {
      return BarConfig{};
    }

    const auto& bars = configService->config().bars;
    const std::string_view panelAnchorBar = configService->config().shell.panelAnchorBar;
    const std::string_view effectiveName = !panelAnchorBar.empty() ? panelAnchorBar : barName;

    const WaylandOutput* wlOutput = nullptr;
    if (platform != nullptr && output != nullptr) {
      wlOutput = platform->findOutputByWl(output);
    }

    const auto resolve = [wlOutput](const BarConfig& bar) {
      return wlOutput != nullptr ? ConfigService::resolveForOutput(bar, *wlOutput) : bar;
    };
    const auto runtimeConfig = [&](std::string_view name) -> std::optional<BarConfig> {
      if (!barConfigProvider || output == nullptr) {
        return std::nullopt;
      }
      return barConfigProvider(output, name);
    };

    if (!effectiveName.empty()) {
      for (const auto& bar : bars) {
        if (bar.name != effectiveName) {
          continue;
        }
        if (auto runtime = runtimeConfig(effectiveName); runtime.has_value()) {
          return runtime;
        }
        BarConfig resolved = resolve(bar);
        if (!resolved.enabled) {
          kLog.warn("panel: bar \"{}\" is disabled on this output; opening without bar attachment", effectiveName);
        }
        return resolved;
      }
      kLog.error("panel: bar \"{}\" not found (source bar or shell.panel_anchor_bar)", effectiveName);
      return std::nullopt;
    }

    if (auto runtime = runtimeConfig({}); runtime.has_value()) {
      return runtime;
    }
    for (const auto& bar : bars) {
      BarConfig resolved = resolve(bar);
      if (resolved.enabled) {
        return resolved;
      }
    }

    // Bar-less output: keep opening panels on floating surfaces rather than aborting.
    return resolve(bars.front());
  }

  bool hasMultipleEnabledBarsOnEdge(
      ConfigService* configService, CompositorPlatform* platform, wl_output* output, std::string_view position
  ) {
    if (configService == nullptr || position.empty()) {
      return false;
    }

    const WaylandOutput* wlOutput = nullptr;
    if (platform != nullptr && output != nullptr) {
      wlOutput = platform->findOutputByWl(output);
    }

    std::size_t count = 0;
    for (const auto& bar : configService->config().bars) {
      const BarConfig resolved = wlOutput != nullptr ? ConfigService::resolveForOutput(bar, *wlOutput) : bar;
      if (!resolved.enabled || resolved.position != position) {
        continue;
      }
      ++count;
      if (count > 1) {
        return true;
      }
    }
    return false;
  }

  // OKARU: los paneles aparecen con un fundido, como las capas de omarchy (fadeLayersIn ≈ 140 ms,
  // fadeLayersOut ≈ 110 ms), en lugar de desplegarse hacia abajo.
  constexpr float kPanelFadeInMs = 140.0F;
  constexpr float kPanelFadeOutMs = 110.0F;
  // El desenfoque del compositor no se puede atenuar: entra cuando el panel ya se ve casi entero.
  constexpr float kPanelBlurFromProgress = 0.6F;

  [[nodiscard]] bool usesConfiguredFloatingLayer(std::string_view panelId) {
    return panelId == "clipboard"
        || panelId == "control-center"
        || panelId == "launcher"
        || panelId == "session"
        || panelId == "setup-wizard"
        || panelId == "wallpaper";
  }

  [[nodiscard]] LayerShellLayer
  resolveFloatingPanelLayer(const ConfigService* configService, std::string_view panelId, const Panel& panel) {
    if (configService != nullptr && usesConfiguredFloatingLayer(panelId)) {
      return layerShellLayerFromConfig(configService->config().shell.panel.floatingLayer);
    }
    return panel.layer();
  }

} // namespace

PanelManager::PanelManager() { s_instance = this; }

PanelManager::~PanelManager() {
  m_captureCloseTimer.stop();
  if (m_captureCloseFrame != nullptr) {
    wl_callback_destroy(m_captureCloseFrame);
  }
  m_persistentHost.setPanelClosedCallback(nullptr);
  if (s_instance == this) {
    s_instance = nullptr;
  }
}

PanelManager& PanelManager::instance() { return *s_instance; }

PanelManager* PanelManager::current() noexcept { return s_instance; }

WaylandConnection* PanelManager::wayland() const noexcept {
  return m_platform != nullptr ? &m_platform->wayland() : nullptr;
}

void PanelManager::initialize(CompositorPlatform& platform, ConfigService* config, RenderContext* renderContext) {
  m_platform = &platform;
  m_config = config;
  m_renderContext = renderContext;
  m_clickShield.initialize(platform.wayland());
  m_persistentHost.initialize(platform, config, renderContext);
}

void PanelManager::setOpenSettingsWindowCallback(std::function<void(std::string)> callback) {
  m_openSettingsWindow = std::move(callback);
}

void PanelManager::setOpenWidgetSettingsCallback(std::function<void(std::string, std::string)> callback) {
  m_openWidgetSettings = std::move(callback);
}

void PanelManager::setOpenPluginSettingsCallback(std::function<bool(std::string)> callback) {
  m_openPluginSettings = std::move(callback);
}

void PanelManager::setCloseSettingsWindowCallback(std::function<void()> callback) {
  m_closeSettingsWindow = std::move(callback);
}

void PanelManager::setToggleSettingsWindowCallback(std::function<void(std::string)> callback) {
  m_toggleSettingsWindow = std::move(callback);
}

void PanelManager::setCloseDesktopWidgetsEditorCallback(std::function<void()> callback) {
  m_closeDesktopWidgetsEditor = std::move(callback);
}

void PanelManager::openSettingsWindow(std::string context) {
  if (isOpen() && !m_closing) {
    closePanel();
  }
  if (m_openSettingsWindow) {
    m_openSettingsWindow(std::move(context));
  }
}

bool PanelManager::openPluginSettings(const std::string& pluginId) {
  if (!m_openPluginSettings) {
    return false;
  }
  if (isOpen() && !m_closing) {
    closePanel();
  }
  return m_openPluginSettings(pluginId);
}

void PanelManager::closeSettingsWindow() {
  if (m_closeSettingsWindow) {
    m_closeSettingsWindow();
  }
}

void PanelManager::toggleSettingsWindow(std::string context) {
  if (isOpen() && !m_closing) {
    closePanel();
  }
  if (m_toggleSettingsWindow) {
    m_toggleSettingsWindow(std::move(context));
    return;
  }
  if (m_openSettingsWindow) {
    m_openSettingsWindow(std::move(context));
  }
}

void PanelManager::setClickShieldExcludeRectsProvider(std::function<std::vector<InputRect>(wl_output*)> provider) {
  m_clickShieldExcludeRectsProvider = std::move(provider);
}

void PanelManager::setFocusGrabBarSurfacesProvider(std::function<std::vector<wl_surface*>()> provider) {
  m_focusGrabBarSurfacesProvider = std::move(provider);
}

void PanelManager::setPanelClosedCallback(std::function<void()> callback) {
  m_panelClosedCallback = std::move(callback);
  m_persistentHost.setPanelClosedCallback([this] {
    if (m_panelClosedCallback) {
      m_panelClosedCallback();
    }
  });
}

void PanelManager::setPanelOpenedCallback(std::function<void()> callback) {
  m_panelOpenedCallback = std::move(callback);
}

void PanelManager::setBarConfigProvider(
    std::function<std::optional<BarConfig>(wl_output*, std::string_view)> provider
) {
  m_barConfigProvider = std::move(provider);
}

void PanelManager::registerPanel(const std::string& id, std::unique_ptr<Panel> content) {
  if (content != nullptr && content->isPersistent()) {
    m_persistentHost.registerPanel(id, std::move(content));
    return;
  }
  m_panels[id] = std::move(content);
}

void PanelManager::unregisterPanel(const std::string& id) {
  if (m_persistentHost.hasPanel(id)) {
    m_persistentHost.unregisterPanel(id);
    return;
  }
  auto it = m_panels.find(id);
  if (it == m_panels.end()) {
    return;
  }
  if (isOpenPanel(id)) {
    if (m_inTransition) {
      // initialize() is still on the stack and destroyPanel would free its surface: keep the
      // panel alive and destroy it from the main loop once the open has returned.
      m_retiredActivePanel = std::move(it->second);
      m_panels.erase(it);
      DeferredCall::callLater([this] {
        if (m_activePanel != nullptr && m_activePanel == m_retiredActivePanel.get()) {
          ++m_destroyGeneration;
          destroyPanel();
        }
        m_retiredActivePanel.reset();
      });
      return;
    }
    // A closing panel is still read by its close animation: destroy it now and cancel
    // the pending deferred destroy.
    ++m_destroyGeneration;
    destroyPanel();
  }
  m_panels.erase(it);
}

void PanelManager::openPanel(const std::string& panelId, PanelOpenRequest request) {
  if (m_inTransition || m_afterCaptureClose) {
    return;
  }

  if (request.output == nullptr && m_platform != nullptr) {
    request.output = m_platform->focusedInteractiveOutput(std::chrono::milliseconds(1200));
    if (request.output == nullptr) {
      // No focus source resolved an output (e.g. a compositor with no focus
      // IPC/backend). Ask the compositor which output an unpinned surface lands
      // on — the focused one — then reopen with that concrete output so all the
      // normal placement (attached, bar-relative, per-output config) applies.
      // Falls back to the arbitrary first output if the probe times out.
      //
      // The open is deferred past this call, so the request's string_view fields
      // are copied into owned storage the continuation keeps alive.
      m_platform->probeFocusedOutput(
          [this, panelId, request, context = std::string(request.context),
           sourceBarName = std::string(request.sourceBarName)](wl_output* probed) mutable {
            request.output =
                probed != nullptr ? probed : m_platform->preferredInteractiveOutput(std::chrono::milliseconds(1200));
            if (request.output == nullptr) {
              return; // no usable output at all — nothing to open on.
            }
            request.context = context;
            request.sourceBarName = sourceBarName;
            openPanel(panelId, request);
          },
          std::chrono::milliseconds(250)
      );
      return;
    }
  }

  // Persistent panels live in their own host: opening one must leave the active
  // panel (and any other persistent panel) alone.
  if (m_persistentHost.hasPanel(panelId)) {
    m_persistentHost.open(panelId, request.output, request.context);
    return;
  }

  if (m_closeDesktopWidgetsEditor) {
    m_closeDesktopWidgetsEditor();
  }

  // If a panel is open or closing, destroy it immediately with no close animation.
  // Bump the generation first so any in-flight deferred destroyPanel is a no-op.
  if (isOpen() || m_closing) {
    ++m_destroyGeneration;
    m_closing = false;
    destroyPanel();
  }

  auto it = m_panels.find(panelId);
  if (it == m_panels.end()) {
    kLog.warn("panel manager: unknown panel \"{}\"", panelId);
    return;
  }

  auto barConfigOpt =
      resolvePanelBarConfig(m_config, m_platform, request.output, m_barConfigProvider, request.sourceBarName);
  if (!barConfigOpt.has_value()) {
    return;
  }
  auto barConfig = std::move(*barConfigOpt);

  m_activePanel = it->second.get();
  m_activePanelId = panelId;
  m_activePanel->setContentScale(shell::panel_surface::contentScale(m_config));
  m_pendingOpenContext = std::string(request.context);
  m_activePanel->setPendingOpenContext(request.context);

  auto panelWidth = static_cast<std::uint32_t>(m_activePanel->preferredWidth());
  auto panelHeight = static_cast<std::uint32_t>(m_activePanel->preferredHeight());
  m_sourceBarName = barConfig.name;
  const bool isBottom = barConfig.position == "bottom";
  const bool isLeft = barConfig.position == "left";
  const bool isRight = barConfig.position == "right";
  const std::int32_t panelGap = m_config->config().shell.panel.floatingOffset;
  const auto screenPadding = static_cast<std::int32_t>(Style::spaceSm);

  std::int32_t resolvedOutputWidth = 0;
  std::int32_t resolvedOutputHeight = 0;
  if (m_platform != nullptr) {
    const auto* wlOutput = m_platform->findOutputByWl(request.output);
    if (wlOutput != nullptr && wlOutput->effectiveLogicalWidth() > 0) {
      resolvedOutputWidth = wlOutput->effectiveLogicalWidth();
    }
    if (wlOutput != nullptr && wlOutput->effectiveLogicalHeight() > 0) {
      resolvedOutputHeight = wlOutput->effectiveLogicalHeight();
    }
  }
  // Backstop clamp: never request a surface larger than the output — the
  // compositor renders such a surface broken. This is sanity capping, not
  // work-area layout; if the compositor still configures smaller (exclusive
  // zones), buildScene lays out at the configured size.
  if (resolvedOutputWidth > 0) {
    panelWidth = std::min(panelWidth, static_cast<std::uint32_t>(std::max(1, resolvedOutputWidth - screenPadding * 2)));
  }
  if (resolvedOutputHeight > 0) {
    panelHeight =
        std::min(panelHeight, static_cast<std::uint32_t>(std::max(1, resolvedOutputHeight - screenPadding * 2)));
  }
  const std::int32_t outputWidth =
      resolvedOutputWidth > 0 ? resolvedOutputWidth : static_cast<std::int32_t>(panelWidth);
  const std::int32_t outputHeight =
      resolvedOutputHeight > 0 ? resolvedOutputHeight : static_cast<std::int32_t>(panelHeight);

  const auto clampMargin = [](float desired, std::int32_t panelSize, std::int32_t outputSize,
                              std::int32_t padding) -> std::int32_t {
    const std::int32_t maxValue = std::max(padding, outputSize - panelSize - padding);
    return static_cast<std::int32_t>(std::clamp(desired, static_cast<float>(padding), static_cast<float>(maxValue)));
  };

  // OKARU: no hay paneles attached; todo menú es flotante.
  const bool fillWidth = m_activePanel->fillsWidth();
  const bool fillHeight = m_activePanel->fillsHeight();
  m_panelFillWidth = fillWidth;
  m_panelFillHeight = fillHeight;
  // A filled axis is sized by the compositor, so it cannot also follow the content.
  m_contentFit = ContentFit{.enabled = m_activePanel->fitsContentHeight() && !fillHeight};
  const bool pluginPanel = m_activePanelId.contains(':');
  // OKARU: desde la barra el menú sale junto al widget ("auto"); sin widget (atajo, IPC), al centro.
  (void)pluginPanel;
  const std::string panelPosition = request.hasAnchorPosition ? "auto" : "center";
  const bool useScreenPosition = panelPosition != "auto" && panelPosition != "center";
  const bool useCenterScreenLayout = panelPosition == "center";
  const bool useFloatingAnchor = !useCenterScreenLayout && request.hasAnchorPosition; // OKARU
  const auto barRect = resolveBarVisibleRect(barConfig, outputWidth, outputHeight);
  // A content-fitted slot reserves the panel's maximum height up front, so it must fit
  // on the panel's side of a horizontal bar; otherwise the placement clamps would push it
  // over the bar. Fixed-size panels keep their size and the clamps as before.
  if (m_contentFit.enabled && !useCenterScreenLayout && !isLeft && !isRight && barConfig.thickness > 0) {
    const std::int32_t room =
        isBottom ? barRect.top - panelGap - screenPadding : outputHeight - barRect.bottom - panelGap - screenPadding;
    panelHeight = std::min(panelHeight, static_cast<std::uint32_t>(std::max(1, room)));
  }
  const auto detachedShadowBleed =
      shell::panel_surface::bleed(m_activePanel->hasDecoration(), m_config->config().shell.shadow);
  const std::uint32_t detachedSurfaceWidth =
      shell::panel_surface::surfaceExtent(panelWidth, detachedShadowBleed.left, detachedShadowBleed.right);
  const std::uint32_t detachedSurfaceHeight =
      shell::panel_surface::surfaceExtent(panelHeight, detachedShadowBleed.up, detachedShadowBleed.down);
  const bool multipleBarsOnEdge =
      hasMultipleEnabledBarsOnEdge(m_config, m_platform, request.output, barConfig.position);
  const bool useReservedEdgePlacement = !useCenterScreenLayout
      && !useScreenPosition
      && multipleBarsOnEdge
      && barConfig.reserveSpace
      && barConfig.thickness > 0;
  // OKARU: con los bordes del widget, el menú se alinea con él; si no, se centra en el punto.
  const float alongStart = request.hasWidgetExtent ? request.widgetStart : (isLeft || isRight ? request.anchorY : request.anchorX);
  const float alongEnd = request.hasWidgetExtent ? request.widgetEnd : alongStart;
  const auto marginLeftFromAnchor = static_cast<std::int32_t>(okaru::panel::alignedMenuStart(
      alongStart, alongEnd, static_cast<float>(panelWidth), static_cast<float>(outputWidth),
      static_cast<float>(screenPadding), request.menuAlign
  ));
  const auto marginTopFromAnchor = static_cast<std::int32_t>(okaru::panel::alignedMenuStart(
      alongStart, alongEnd, static_cast<float>(panelHeight), static_cast<float>(outputHeight),
      static_cast<float>(screenPadding), request.menuAlign
  ));

  std::uint32_t standaloneAnchor = 0;
  std::int32_t standaloneMarginTop = 0;
  std::int32_t standaloneMarginRight = 0;
  std::int32_t standaloneMarginBottom = 0;
  std::int32_t standaloneMarginLeft = 0;
  if (!useCenterScreenLayout) {
    const std::int32_t barWidth = std::max(0, barRect.right - barRect.left);
    const std::int32_t barHeight = std::max(0, barRect.bottom - barRect.top);
    const auto centeredAlongBarX = clampMargin(
        static_cast<float>(barRect.left) + (static_cast<float>(barWidth) - static_cast<float>(panelWidth)) * 0.5F,
        static_cast<std::int32_t>(panelWidth), outputWidth, screenPadding
    );
    const auto centeredAlongBarY = clampMargin(
        static_cast<float>(barRect.top) + (static_cast<float>(barHeight) - static_cast<float>(panelHeight)) * 0.5F,
        static_cast<std::int32_t>(panelHeight), outputHeight, screenPadding
    );

    if (useScreenPosition) {
      // Pinned to a screen edge/corner, independent of the bar.
      const auto sp = shell::screenPositionAnchor(panelPosition, panelGap);
      standaloneAnchor = sp.anchor;
      standaloneMarginTop = sp.marginTop;
      standaloneMarginRight = sp.marginRight;
      standaloneMarginBottom = sp.marginBottom;
      standaloneMarginLeft = sp.marginLeft;
    } else if (useReservedEdgePlacement) {
      if (isLeft) {
        standaloneAnchor = LayerShellAnchor::Left | LayerShellAnchor::Top;
        standaloneMarginLeft = panelGap;
        standaloneMarginTop = useFloatingAnchor ? marginTopFromAnchor : centeredAlongBarY;
      } else if (isRight) {
        standaloneAnchor = LayerShellAnchor::Right | LayerShellAnchor::Top;
        standaloneMarginRight = panelGap;
        standaloneMarginTop = useFloatingAnchor ? marginTopFromAnchor : centeredAlongBarY;
      } else if (isBottom) {
        standaloneAnchor = LayerShellAnchor::Bottom | LayerShellAnchor::Left;
        standaloneMarginBottom = panelGap;
        standaloneMarginLeft = useFloatingAnchor ? marginLeftFromAnchor : centeredAlongBarX;
      } else {
        standaloneAnchor = LayerShellAnchor::Top | LayerShellAnchor::Left;
        standaloneMarginTop = panelGap;
        standaloneMarginLeft = useFloatingAnchor ? marginLeftFromAnchor : centeredAlongBarX;
      }
    } else {
      standaloneAnchor = LayerShellAnchor::Top | LayerShellAnchor::Left;
      if (isLeft) {
        standaloneMarginLeft = clampMargin(
            static_cast<float>(barRect.right + panelGap), static_cast<std::int32_t>(panelWidth), outputWidth,
            screenPadding
        );
        standaloneMarginTop = useFloatingAnchor ? marginTopFromAnchor : centeredAlongBarY;
      } else if (isRight) {
        standaloneMarginLeft = clampMargin(
            static_cast<float>(barRect.left - static_cast<std::int32_t>(panelWidth) - panelGap),
            static_cast<std::int32_t>(panelWidth), outputWidth, screenPadding
        );
        standaloneMarginTop = useFloatingAnchor ? marginTopFromAnchor : centeredAlongBarY;
      } else if (isBottom) {
        standaloneMarginTop = clampMargin(
            static_cast<float>(barRect.top - static_cast<std::int32_t>(panelHeight) - panelGap),
            static_cast<std::int32_t>(panelHeight), outputHeight, screenPadding
        );
        standaloneMarginLeft = useFloatingAnchor ? marginLeftFromAnchor : centeredAlongBarX;
      } else {
        standaloneMarginTop = clampMargin(
            static_cast<float>(barRect.bottom + panelGap), static_cast<std::int32_t>(panelHeight), outputHeight,
            screenPadding
        );
        standaloneMarginLeft = useFloatingAnchor ? marginLeftFromAnchor : centeredAlongBarX;
      }
    }
  }
  // Which slot edge a content-fitted card keeps: the one this placement anchors to.
  CardAlign detachedCardAlign = CardAlign::Start;
  if (useCenterScreenLayout) {
    detachedCardAlign = CardAlign::Center;
  } else if (useScreenPosition) {
    const bool anchoredTop = (standaloneAnchor & LayerShellAnchor::Top) != 0;
    const bool anchoredBottom = (standaloneAnchor & LayerShellAnchor::Bottom) != 0;
    detachedCardAlign = anchoredTop == anchoredBottom ? CardAlign::Center
        : anchoredTop                                 ? CardAlign::Start
                                                      : CardAlign::End;
  } else if (isBottom) {
    detachedCardAlign = CardAlign::End;
  } else if (isLeft || isRight) {
    detachedCardAlign = CardAlign::Center;
  }

  if (useCenterScreenLayout) {
    standaloneAnchor = LayerShellAnchor::Top | LayerShellAnchor::Left;
    standaloneMarginLeft = (outputWidth - static_cast<std::int32_t>(panelWidth)) / 2 - detachedShadowBleed.left;
    standaloneMarginTop = (outputHeight - static_cast<std::int32_t>(panelHeight)) / 2 - detachedShadowBleed.up;
  } else {
    if ((standaloneAnchor & LayerShellAnchor::Left) != 0) {
      standaloneMarginLeft -= detachedShadowBleed.left;
    } else if ((standaloneAnchor & LayerShellAnchor::Right) != 0) {
      standaloneMarginRight -= detachedShadowBleed.right;
    }
    if ((standaloneAnchor & LayerShellAnchor::Top) != 0) {
      standaloneMarginTop -= detachedShadowBleed.up;
    } else if ((standaloneAnchor & LayerShellAnchor::Bottom) != 0) {
      standaloneMarginBottom -= detachedShadowBleed.down;
    }
  }

  InputRect detachedPanelInputRect = panelInputRectForSurface(
      standaloneAnchor, standaloneMarginTop, standaloneMarginRight, standaloneMarginBottom, standaloneMarginLeft,
      outputWidth, outputHeight, detachedSurfaceWidth, detachedSurfaceHeight, detachedShadowBleed.left,
      detachedShadowBleed.up, panelWidth, panelHeight
  );

  // Single-bar detached panels are placed relative to the bar's config edge. Honor
  // other surfaces' exclusive zones (exclusive_zone = 0 below) and anchor to the
  // bar's reserved edge so the panel tracks the bar's real on-screen position;
  // subtract the bar's own reservation on the main axis to avoid double-counting.
  // Reproduces the prior absolute placement when nothing else reserves space.
  const bool useBarRelativeDetached = !useCenterScreenLayout && !useScreenPosition && !useReservedEdgePlacement;
  if (useBarRelativeDetached) {
    const std::int32_t barReserved =
        barConfig.reserveSpace ? reservedBarEdgeDistance(barConfig, m_config->config().shell.shadow) : 0;
    const auto sw = static_cast<std::int32_t>(detachedSurfaceWidth);
    const auto sh = static_cast<std::int32_t>(detachedSurfaceHeight);
    if (isBottom) {
      standaloneAnchor = LayerShellAnchor::Bottom | LayerShellAnchor::Left;
      standaloneMarginBottom = outputHeight - sh - standaloneMarginTop - barReserved;
      standaloneMarginTop = 0;
    } else if (isRight) {
      standaloneAnchor = LayerShellAnchor::Top | LayerShellAnchor::Right;
      standaloneMarginRight = outputWidth - sw - standaloneMarginLeft - barReserved;
      standaloneMarginLeft = 0;
    } else if (isLeft) {
      standaloneMarginLeft -= barReserved;
    } else {
      standaloneMarginTop -= barReserved;
    }
  }

  // A filled axis dual-anchors the surface with a requested size of 0: the
  // compositor assigns the extent, subtracting every exclusive zone on the
  // output (all bars and any third-party client) — the shell never computes
  // the work area itself. Margins keep the screen padding around the visible
  // body (the shadow bleed sits outside the padding); they override whatever
  // the placement branches above computed on that axis. The default size is
  // only the fallback if the compositor assigns nothing.
  std::uint32_t requestedSurfaceWidth = detachedSurfaceWidth;
  std::uint32_t requestedSurfaceHeight = detachedSurfaceHeight;
  std::uint32_t fallbackSurfaceWidth = detachedSurfaceWidth;
  std::uint32_t fallbackSurfaceHeight = detachedSurfaceHeight;
  if (fillWidth) {
    standaloneAnchor |= LayerShellAnchor::Left | LayerShellAnchor::Right;
    standaloneMarginLeft = screenPadding - detachedShadowBleed.left;
    standaloneMarginRight = screenPadding - detachedShadowBleed.right;
    requestedSurfaceWidth = 0;
    fallbackSurfaceWidth =
        static_cast<std::uint32_t>(std::max(1, outputWidth - standaloneMarginLeft - standaloneMarginRight));
    detachedPanelInputRect.x = screenPadding;
    detachedPanelInputRect.width = std::max(1, outputWidth - screenPadding * 2);
  }
  if (fillHeight) {
    standaloneAnchor |= LayerShellAnchor::Top | LayerShellAnchor::Bottom;
    standaloneMarginTop = screenPadding - detachedShadowBleed.up;
    standaloneMarginBottom = screenPadding - detachedShadowBleed.down;
    requestedSurfaceHeight = 0;
    fallbackSurfaceHeight =
        static_cast<std::uint32_t>(std::max(1, outputHeight - standaloneMarginTop - standaloneMarginBottom));
    detachedPanelInputRect.y = screenPadding;
    detachedPanelInputRect.height = std::max(1, outputHeight - screenPadding * 2);
  }

  const LayerShellLayer floatingPanelLayer = resolveFloatingPanelLayer(m_config, m_activePanelId, *m_activePanel);
  const LayerShellLayer panelLayer = floatingPanelLayer;
  m_panelLayer = panelLayer;

  const bool hasFocusGrab =
      m_platform != nullptr && m_platform->focusGrabService() != nullptr && m_platform->focusGrabService()->available();
  // Neither outside-click mechanism works for a panel that must not take keyboard
  // focus: the click shield swallows the click meant for the app below, and the
  // compositor focus grab takes the keyboard.
  const bool wantsOutsideDismiss =
      m_activePanel->dismissOnOutsideClick() && m_activePanel->keyboardMode() != LayerShellKeyboard::None;
  const PanelKeyboardPlan keyboardPlan =
      resolvePanelKeyboardPlan(m_activePanel->keyboardMode(), hasFocusGrab, hasFocusGrab && wantsOutsideDismiss, false);

  if (wantsOutsideDismiss) {
    activateClickShield(panelLayer);
  }

  auto surfaceConfig = LayerSurfaceConfig{
      .nameSpace = "okaru-panel",
      .layer = floatingPanelLayer,
      .anchor = standaloneAnchor,
      .width = requestedSurfaceWidth,
      .height = requestedSurfaceHeight,
      // Floating panels at the center position ignore exclusive zones; filled axes
      // must respect them so the compositor subtracts bars and other clients.
      .exclusiveZone = useCenterScreenLayout && !fillWidth && !fillHeight ? -1 : 0,
      .marginTop = standaloneMarginTop,
      .marginRight = standaloneMarginRight,
      .marginBottom = standaloneMarginBottom,
      .marginLeft = standaloneMarginLeft,
      .keyboard = keyboardPlan.initial,
      .defaultWidth = fallbackSurfaceWidth,
      .defaultHeight = fallbackSurfaceHeight,
      .prewarmBlur = true,
  };

  const auto configureSurfaceCallbacks = [this](Surface& surface) {
    surface.setRenderContext(m_renderContext);
    surface.setConfigureCallback([this](std::uint32_t /*width*/, std::uint32_t /*height*/) {
      if (m_surface != nullptr) {
        m_surface->requestLayout();
      }
    });
    surface.setPrepareFrameCallback([this](bool needsUpdate, bool needsLayout) {
      prepareFrame(needsUpdate, needsLayout);
    });
    surface.setFrameTickCallback([this](float deltaMs) {
      if (m_activePanel != nullptr) {
        m_activePanel->onFrameTick(deltaMs);
      }
    });
    surface.setAnimationManager(&m_animations);
  };

  const auto resetPanelOpenState = [this] {
    deactivateOutsideClickHandlers();
    m_surface.reset();
    m_layerSurface = nullptr;
    m_output = nullptr;
    m_wlSurface = nullptr;
    m_panelLayer = LayerShellLayer::Top;
    m_activePanel = nullptr;
    m_activePanelId.clear();
    m_pendingOpenContext.clear();
    m_panelInsetX = 0;
    m_panelInsetY = 0;
    m_panelVisualWidth = 0;
    m_panelVisualHeight = 0;
    m_panelOutputInputRect.reset();
    m_panelFillWidth = false;
    m_panelFillHeight = false;
    m_detachedBleedRight = 0;
    m_detachedBleedBottom = 0;
    m_detachedRevealProgress = 1.0F;
    m_keyboardRelaxTimer.stop();
    m_sourceBarName.clear();
    m_contentFit = {};
  };


  auto layerSurface = std::make_unique<LayerSurface>(m_platform->wayland(), std::move(surfaceConfig));
  m_layerSurface = layerSurface.get();
  m_surface = std::move(layerSurface);
  m_panelInsetX = detachedShadowBleed.left;
  m_panelInsetY = detachedShadowBleed.up;
  m_panelVisualWidth = panelWidth;
  m_panelVisualHeight = panelHeight;
  m_contentFit.align = detachedCardAlign;
  m_contentFit.slotInsetY = m_panelInsetY;
  m_contentFit.slotHeight = panelHeight;
  m_contentFit.slotOutputY = detachedPanelInputRect.y;
  m_detachedBleedRight = detachedShadowBleed.right;
  m_detachedBleedBottom = detachedShadowBleed.down;
  // This path publishes the compositor blur region before the first scene build.
  // Keep detached panels hidden until buildScene applies the opening reveal.
  m_detachedRevealProgress = 0.0F;
  configureSurfaceCallbacks(*m_surface);
  if (wantsOutsideDismiss) {
    m_panelOutputInputRect = detachedPanelInputRect;
    m_clickShield.setPanelInputRect(request.output, *m_panelOutputInputRect);
  }

  // Guard against re-entrancy: initialize can process queued Wayland events.
  m_inTransition = true;
  bool ok = m_layerSurface->initialize(request.output);
  m_inTransition = false;

  if (!ok) {
    kLog.warn("panel manager: failed to initialize surface for panel \"{}\"", panelId);
    resetPanelOpenState();
    return;
  }

  m_output = request.output;
  m_wlSurface = m_surface->wlSurface();
  m_surface->setInputRegion(
      {InputRect{m_panelInsetX, m_panelInsetY, static_cast<int>(panelWidth), static_cast<int>(panelHeight)}}
  );
  m_surface->setBlurRegion({});
  // Activate outside-click dismissal (focus grab or click shield).
  if (hasFocusGrab && wantsOutsideDismiss) {
    activateFocusGrab();
  }
  if (keyboardPlan.relaxed.has_value()) {
    const std::uint64_t gen = m_destroyGeneration;
    const LayerShellKeyboard relaxed = *keyboardPlan.relaxed;
    m_keyboardRelaxTimer.start(kKeyboardRelaxDelay, [this, gen, relaxed] {
      if (m_destroyGeneration != gen || m_layerSurface == nullptr || m_closing) {
        return;
      }
      applyKeyboardRelaxation(relaxed);
    });
  }
  kLog.debug("panel manager: opened \"{}\"", panelId);
  if (m_panelOpenedCallback) {
    m_panelOpenedCallback();
  }
}

void PanelManager::activateClickShield(LayerShellLayer layer) {
  if (m_activePanel == nullptr || m_platform == nullptr) {
    return;
  }
  // Hyprland: prefer the native focus-grab path. Skip the shield and let
  // activateFocusGrab handle it later.
  auto* grabService = m_platform->focusGrabService();
  if (grabService != nullptr && grabService->available()) {
    return;
  }
  std::vector<wl_output*> outputs;
  outputs.reserve(m_platform->outputs().size());
  for (const auto& wlOutput : m_platform->outputs()) {
    if (wlOutput.output != nullptr) {
      outputs.push_back(wlOutput.output);
    }
  }
  m_clickShield.activate(outputs, layer, m_clickShieldExcludeRectsProvider);
}

void PanelManager::activateFocusGrab() {
  if (m_platform == nullptr || m_wlSurface == nullptr) {
    return;
  }
  auto* grabService = m_platform->focusGrabService();
  if (grabService == nullptr || !grabService->available()) {
    return;
  }
  // Whitelist the panel, bars, and panel-owned popups. Clicks on whitelisted
  // surfaces pass through normally. Clicks anywhere else clear the grab and
  // close the panel.
  m_focusGrab = grabService->createGrab();
  if (m_focusGrab == nullptr) {
    return;
  }
  m_focusGrab->setOnCleared([this] {
    if (isOpen() && !m_closing) {
      closePanel();
    }
  });
  grabService->setPopupGrabHost(this);

  // Start with the panel as the sole surface so the compositor cannot choose a
  // bar as the initial keyboard target.
  m_focusGrab->addSurface(m_wlSurface);
  m_focusGrab->commit();
  addFocusGrabWhitelistSurfaces(*m_focusGrab, m_wlSurface);
  m_focusGrab->commit();
}

void PanelManager::applyKeyboardRelaxation(LayerShellKeyboard mode) {
  if (m_layerSurface == nullptr) {
    return;
  }
  if (m_focusGrab == nullptr) {
    m_layerSurface->setKeyboardInteractivity(mode);
    return;
  }
  if (m_platform == nullptr || m_wlSurface == nullptr) {
    return;
  }

  auto* grabService = m_platform->focusGrabService();
  if (grabService == nullptr || !grabService->available()) {
    return;
  }

  auto replacement = grabService->createGrab();
  if (replacement == nullptr) {
    return;
  }
  replacement->setOnCleared([this] {
    if (isOpen() && !m_closing) {
      closePanel();
    }
  });

  wl_surface* focusSurface = m_wlSurface;
  if (wl_surface* current = m_platform->lastKeyboardSurface(); isFocusGrabWhitelistSurface(current)) {
    focusSurface = current;
  }
  replacement->addSurface(focusSurface);

  // A fresh grab must start after the layer commit so its keyboard refocus wins
  // over compositors applying follow-mouse focus during Exclusive -> OnDemand.
  m_focusGrab.reset();
  m_layerSurface->setKeyboardInteractivity(mode);
  m_focusGrab = std::move(replacement);
  grabService->setPopupGrabHost(this);

  // Keep the first commit limited to the previous focus surface. The
  // compositor may choose any surface when a grab first becomes active.
  m_focusGrab->commit();

  addFocusGrabWhitelistSurfaces(*m_focusGrab, focusSurface);
  m_focusGrab->commit();
}

void PanelManager::addFocusGrabWhitelistSurfaces(FocusGrab& grab, wl_surface* excludedSurface) {
  const auto addSurface = [&grab, excludedSurface](wl_surface* surface) {
    if (surface != nullptr && surface != excludedSurface) {
      grab.addSurface(surface);
    }
  };

  addSurface(m_wlSurface);
  if (m_focusGrabBarSurfacesProvider) {
    auto bars = m_focusGrabBarSurfacesProvider();
    for (auto* surface : bars) {
      addSurface(surface);
    }
  }
  for (auto* surface : m_focusGrabPopupSurfaces) {
    addSurface(surface);
  }
}

bool PanelManager::isFocusGrabWhitelistSurface(wl_surface* surface) const {
  if (surface == nullptr) {
    return false;
  }
  if (surface == m_wlSurface || m_focusGrabPopupSurfaces.contains(surface)) {
    return true;
  }
  if (m_focusGrabBarSurfacesProvider == nullptr) {
    return false;
  }
  const auto bars = m_focusGrabBarSurfacesProvider();
  return std::ranges::find(bars, surface) != bars.end();
}

void PanelManager::deactivateOutsideClickHandlers() {
  m_clickShield.deactivate();
  if (m_platform != nullptr) {
    if (auto* svc = m_platform->focusGrabService(); svc != nullptr && svc->popupGrabHost() == this) {
      svc->setPopupGrabHost(nullptr);
    }
  }
  m_focusGrab.reset();
  m_focusGrabPopupSurfaces.clear();
}

void PanelManager::closePanel(bool animateClose) {
  if (!isOpen() || m_inTransition || m_closing) {
    return;
  }

  kLog.debug("panel manager: closing \"{}\"", m_activePanelId);

  // Drop the outside-click handlers as soon as close starts.
  // During the close animation we want clicks on apps to behave normally.
  deactivateOutsideClickHandlers();

  // Disable input during close animation
  m_inputDispatcher.setSceneRoot(nullptr);
  m_closing = true;

  if (animateClose && m_sceneRoot != nullptr && m_activePanel != nullptr && m_activePanel->wantsCloseAnimation()) {
    const std::uint64_t gen = ++m_destroyGeneration;
    m_animations.cancelForOwner(m_sceneRoot.get());
    m_animations.animate(
        m_detachedRevealProgress, 0.0F, kPanelFadeOutMs, Easing::Linear,
        [this](float v) { applyDetachedReveal(v); },
        [this, gen] {
          DeferredCall::callLater([this, gen] {
            if (m_destroyGeneration == gen) {
              destroyPanel();
            }
          });
        },
        m_sceneRoot.get()
    );
    m_surface->requestRedraw();
  } else {
    destroyPanel();
  }
}

void PanelManager::closePanelForCapture(std::function<void()> afterClose) {
  if (m_afterCaptureClose) {
    return;
  }
  if (!isOpen() || m_surface == nullptr || m_wlSurface == nullptr || m_inTransition) {
    closePanel(/*animateClose=*/false);
    DeferredCall::callLater(std::move(afterClose));
    return;
  }

  m_afterCaptureClose = std::move(afterClose);
  m_closing = true;
  const std::uint64_t generation = ++m_destroyGeneration;
  deactivateOutsideClickHandlers();
  m_inputDispatcher.setSceneRoot(nullptr);
  TooltipManager::instance().forceDestroy();
  if (m_activePopup != nullptr) {
    m_activePopup->close();
    m_activePopup = nullptr;
  }
  m_animations.cancelAll();
  m_keyboardRelaxTimer.stop();
  m_surface->setPrepareFrameCallback({});
  m_surface->setFrameTickCallback({});
  m_surface->setUpdateCallback({});
  m_surface->setSceneRoot(nullptr);
  m_surface->clearBlurRegion();
  m_surface->setOpaque(false);
  m_surface->setInputRegion({});
  m_surface->discardPendingFrameCallback();

  static const wl_callback_listener listener = {.done = &PanelManager::handleCaptureCloseFrame};
  m_captureCloseFrame = wl_surface_frame(m_wlSurface);
  if (m_captureCloseFrame == nullptr || wl_callback_add_listener(m_captureCloseFrame, &listener, this) != 0) {
    kLog.warn("could not wait for the panel's clear frame; closing before capture");
    destroyPanel();
    return;
  }

  m_captureCloseTimer.start(kCaptureCloseTimeout, [this, generation] {
    if (m_destroyGeneration == generation && m_closing) {
      kLog.warn("panel clear frame timed out; closing before capture");
      destroyPanel();
    }
  });
  // The caller defers this handoff out of input dispatch. Submit the clear buffer
  // now so no intervening state-only commit can claim its frame callback.
  m_surface->renderNow();
}

void PanelManager::handleCaptureCloseFrame(void* data, wl_callback* callback, std::uint32_t /*time*/) {
  auto* self = static_cast<PanelManager*>(data);
  if (callback != self->m_captureCloseFrame) {
    return;
  }
  wl_callback_destroy(callback);
  self->m_captureCloseFrame = nullptr;
  self->m_captureCloseTimer.stop();
  const std::uint64_t generation = self->m_destroyGeneration;
  DeferredCall::callLater([self, generation] {
    if (self->m_destroyGeneration == generation && self->m_closing) {
      self->destroyPanel();
    }
  });
}

void PanelManager::destroyPanel() {
  m_captureCloseTimer.stop();
  if (m_captureCloseFrame != nullptr) {
    wl_callback_destroy(m_captureCloseFrame);
    m_captureCloseFrame = nullptr;
  }
  // Defensive: closePanel deactivates first, but destroyPanel can also be
  // reached directly when openPanel preempts an open panel.
  deactivateOutsideClickHandlers();
  m_animations.cancelAll();
  m_closing = false;
  m_pointerInside = false;
  m_attachedPopupCount = 0;
  m_inputDispatcher.setSceneRoot(nullptr);
  // Hover leave only fades tooltips asynchronously. Destroy them (and any
  // open context menu) before the layer surface — xdg_popup must die first.
  TooltipManager::instance().restoreBarTooltipsForPanel(m_activePanelId);
  TooltipManager::instance().forceDestroy();
  if (m_activePopup != nullptr) {
    m_activePopup->close();
    m_activePopup = nullptr;
  }
  if (m_activePanel != nullptr) {
    m_activePanel->onClose();
  }
  m_bgNode = nullptr;
  m_contentNode = nullptr;
  m_detachedRevealClipNode = nullptr;
  m_detachedRevealContentNode = nullptr;
  m_panelShadowNode = nullptr;
  m_selectPopup.reset();
  m_sceneRoot.reset();
  m_surface.reset();
  m_layerSurface = nullptr;
  m_output = nullptr;
  m_wlSurface = nullptr;
  m_activePanel = nullptr;
  m_activePanelId.clear();
  m_pendingOpenContext.clear();
  m_panelInsetX = 0;
  m_panelInsetY = 0;
  m_panelVisualWidth = 0;
  m_panelVisualHeight = 0;
  m_panelOutputInputRect.reset();
  m_panelFillWidth = false;
  m_panelFillHeight = false;
  m_detachedBleedRight = 0;
  m_detachedBleedBottom = 0;
  m_detachedRevealProgress = 1.0F;
  m_keyboardRelaxTimer.stop();
  m_sourceBarName.clear();
  m_contentFit = {};
  if (m_platform != nullptr) {
    m_platform->stopKeyRepeat();
  }
  if (m_panelClosedCallback) {
    m_panelClosedCallback();
  }
  if (m_afterCaptureClose) {
    m_afterCaptureClose();
    m_afterCaptureClose = {};
  }
}

void PanelManager::togglePanel(const std::string& panelId, PanelOpenRequest request) {
  if (m_persistentHost.hasPanel(panelId)) {
    if (m_persistentHost.isOpen(panelId)) {
      m_persistentHost.close(panelId);
      return;
    }
    openPanel(panelId, request);
    return;
  }
  // Treat a closing panel as closed: re-clicking while it animates out reopens it immediately.
  if (isOpen() && !m_closing && m_activePanelId == panelId) {
    if (!request.context.empty() && m_activePanel != nullptr) {
      if (m_activePanel->isContextActive(request.context)) {
        closePanel();
        return;
      }
      // Panels placed near the clicked widget must fully reopen so geometry
      // and bar decoration track the new anchor.
      if (request.hasAnchorPosition) { // OKARU: todo menú de la barra sigue a su widget
        openPanel(panelId, request);
        return;
      }
      m_activePanel->onOpen(request.context);
      refresh();
      return;
    }
    closePanel();
  } else {
    openPanel(panelId, request);
  }
}

void PanelManager::togglePanel(const std::string& panelId) {
  if (m_persistentHost.hasPanel(panelId)) {
    if (m_persistentHost.isOpen(panelId)) {
      m_persistentHost.close(panelId);
      return;
    }
    openPanel(panelId, PanelOpenRequest{});
    return;
  }
  if (isOpen() && !m_closing && m_activePanelId == panelId) {
    closePanel();
    return;
  }
  // Output left unset: openPanel resolves it (focus source, else compositor probe).
  openPanel(panelId, PanelOpenRequest{});
}

bool PanelManager::onPointerEvent(const PointerEvent& event) {
  const bool ignorePointer = (m_activePanel != nullptr && m_activePanel->ignorePointerEvent());

  // A context menu may belong to a persistent plugin panel, for which the
  // ordinary single-panel host is closed. Route the grabbing popup first.
  if (m_activePopup != nullptr) {
    const bool onPopup = (event.surface != nullptr && event.surface == m_activePopup->wlSurface());
    if (ignorePointer && onPopup) {
      return true;
    }
    if (m_activePopup->onPointerEvent(event)) {
      return true;
    }
    if (event.type == PointerEvent::Type::Button && event.pressed) {
      m_activePopup->close();
      return true;
    }
  }

  // Persistent panels own separate surfaces; the host claims only its own.
  if (m_persistentHost.onPointerEvent(event)) {
    return true;
  }
  if (!isOpen() || m_inTransition) {
    return false;
  }

  if (m_selectPopup != nullptr && m_selectPopup->isSelectDropdownOpen()) {
    const bool onSelect = (event.surface != nullptr && event.surface == m_selectPopup->wlSurface());
    if (ignorePointer && onSelect) {
      return true;
    }
    if (m_selectPopup->onPointerEvent(event)) {
      return true;
    }
    if (event.type == PointerEvent::Type::Button && event.pressed) {
      m_selectPopup->closeSelectDropdown();
      return true;
    }
  }

  if (m_attachedPopupCount > 0) {
    if (event.surface == m_wlSurface) {
      if (event.type == PointerEvent::Type::Enter) {
        m_pointerInside = true;
      } else if (event.type == PointerEvent::Type::Leave) {
        m_pointerInside = false;
      }
    }
    return false;
  }

  switch (event.type) {
  case PointerEvent::Type::Enter: {
    if (event.surface == m_wlSurface) {
      m_pointerInside = true;
      if (!ignorePointer) {
        m_inputDispatcher.pointerEnter(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
      }
    }
    break;
  }
  case PointerEvent::Type::Leave: {
    if (event.surface == m_wlSurface) {
      m_pointerInside = false;
      if (!ignorePointer) {
        m_inputDispatcher.pointerLeave();
      }
    }
    break;
  }
  case PointerEvent::Type::Motion: {
    if (!m_pointerInside || ignorePointer) {
      return false;
    }
    m_inputDispatcher.pointerMotion(static_cast<float>(event.sx), static_cast<float>(event.sy), 0);
    break;
  }
  case PointerEvent::Type::Button: {
    bool pressed = event.pressed;

    // Click outside panel closes it.
    if (pressed && !m_pointerInside) {
      closePanel();
      return false;
    }

    if (m_pointerInside) {
      if (ignorePointer) {
        return true;
      }
      if (pressed
          && event.surface == m_wlSurface
          && m_inputDispatcher.hoveredArea() == nullptr
          && m_activePanel != nullptr
          && m_activePanel->dismissTransientUi()) {
        refresh();
        return true;
      }

      m_inputDispatcher.pointerButton(
          static_cast<float>(event.sx), static_cast<float>(event.sy), event.button, pressed, event.serial, event.time,
          event.touch
      );
    }
    break;
  }
  case PointerEvent::Type::Axis: {
    if (!m_pointerInside || ignorePointer) {
      return false;
    }
    m_inputDispatcher.pointerAxis(
        static_cast<float>(event.sx), static_cast<float>(event.sy), event.axis, event.axisSource, event.axisValue,
        event.axisDiscrete, event.axisValue120, event.axisLines
    );
    break;
  }
  }

  // Pointer interactions often only affect visual state.
  // Relayout only when the scene explicitly accumulated layout invalidation.
  if (m_surface != nullptr && m_sceneRoot != nullptr && (m_sceneRoot->paintDirty() || m_sceneRoot->layoutDirty())) {
    if (m_sceneRoot->layoutDirty() && m_activePanel != nullptr && !m_activePanel->deferPointerRelayout()) {
      m_surface->requestLayout();
    } else {
      m_surface->requestRedraw();
    }
  }

  return m_pointerInside;
}

bool PanelManager::isOpen() const noexcept { return m_surface != nullptr && m_activePanel != nullptr; }

bool PanelManager::isOpenPanel(std::string_view panelId) const noexcept {
  if (m_persistentHost.hasPanel(panelId)) {
    return m_persistentHost.isOpen(panelId);
  }
  return isOpen() && m_activePanelId == panelId;
}

bool PanelManager::isPanelTransitionActive() const noexcept {
  if (!isOpen() && !m_closing) {
    return false;
  }
  if (m_closing) {
    return true;
  }
  return m_detachedRevealProgress < 0.999F;
}

wl_output* PanelManager::attachedPanelOutput() const noexcept { return m_output; }

std::string_view PanelManager::attachedSourceBarName() const noexcept { return m_sourceBarName; }

const std::string& PanelManager::activePanelId() const noexcept { return m_activePanelId; }

bool PanelManager::isActivePanelContext(std::string_view context) const noexcept {
  if (!isOpen() || m_activePanel == nullptr) {
    return false;
  }
  return m_activePanel->isContextActive(context);
}

void PanelManager::refresh() {
  m_persistentHost.refresh();
  if (!isOpen() || m_renderContext == nullptr || m_activePanel == nullptr || m_surface == nullptr) {
    return;
  }
  if (m_activePanel->deferExternalRefresh()) {
    return;
  }

  m_surface->requestUpdate();
}

void PanelManager::refreshPanel(std::string_view panelId) {
  if (m_persistentHost.hasPanel(panelId)) {
    m_persistentHost.refreshPanel(panelId);
    return;
  }
  if (isOpenPanel(panelId)) {
    refresh();
  }
}

void PanelManager::closePanelById(std::string_view panelId) {
  if (m_persistentHost.hasPanel(panelId)) {
    m_persistentHost.close(std::string(panelId));
    return;
  }
  if (isOpenPanel(panelId)) {
    closePanel();
  }
}

void PanelManager::requestAnimationFrameForPanel(std::string_view panelId) {
  if (m_persistentHost.hasPanel(panelId)) {
    m_persistentHost.requestAnimationFrame(panelId);
    return;
  }
  if (isOpenPanel(panelId)) {
    requestRedraw();
  }
}

void PanelManager::onIconThemeChanged() {
  m_persistentHost.onIconThemeChanged();
  if (!isOpen() || m_activePanel == nullptr || m_surface == nullptr) {
    return;
  }

  m_activePanel->onIconThemeChanged();
  m_surface->requestUpdate();
}

void PanelManager::focusArea(InputArea* area) {
  if (!isOpen() || m_sceneRoot == nullptr) {
    return;
  }
  m_inputDispatcher.setFocus(area);
}

void PanelManager::requestUpdateOnly() {
  if (!isOpen() || m_surface == nullptr) {
    return;
  }
  m_surface->requestUpdateOnly();
}

void PanelManager::requestLayout() {
  if (!isOpen() || m_surface == nullptr) {
    return;
  }
  m_surface->requestLayout();
}

void PanelManager::requestRedraw() {
  if (!isOpen() || m_surface == nullptr) {
    return;
  }
  m_surface->requestRedraw();
}

void PanelManager::requestFrameTick() {
  if (!isOpen() || m_surface == nullptr) {
    return;
  }
  m_surface->requestFrameTick();
}

void PanelManager::close() { closePanel(); }

void PanelManager::configureContextMenuPopup(ContextMenuPopup& popup) const {
  if (m_config != nullptr) {
    popup.setShadowConfig(m_config->config().shell.shadow);
  }
}

void PanelManager::setActivePopup(ContextMenuPopup* popup) {
  if (m_selectPopup != nullptr && m_selectPopup->isSelectDropdownOpen()) {
    m_selectPopup->closeSelectDropdown();
  }
  if (m_activePopup != nullptr && m_activePopup != popup) {
    m_activePopup->close();
  }
  m_activePopup = popup;
}

void PanelManager::clearActivePopup() { m_activePopup = nullptr; }

void PanelManager::registerPopupSurface(wl_surface* surface) {
  if (surface == nullptr) {
    return;
  }
  const bool inserted = m_focusGrabPopupSurfaces.insert(surface).second;
  if (!inserted || m_focusGrab == nullptr) {
    return;
  }
  m_focusGrab->addSurface(surface);
  m_focusGrab->commit();
}

void PanelManager::unregisterPopupSurface(wl_surface* surface) {
  if (surface == nullptr) {
    return;
  }
  const bool removed = m_focusGrabPopupSurfaces.erase(surface) != 0;
  if (!removed || m_focusGrab == nullptr) {
    return;
  }
  m_focusGrab->removeSurface(surface);
  m_focusGrab->commit();
}

void PanelManager::beginAttachedPopup(wl_surface* surface) {
  if (surface == nullptr || surface != m_wlSurface) {
    return;
  }
  ++m_attachedPopupCount;
}

void PanelManager::endAttachedPopup(wl_surface* surface) {
  if (surface == nullptr || surface != m_wlSurface) {
    return;
  }
  if (m_attachedPopupCount > 0) {
    --m_attachedPopupCount;
  }
  if (m_attachedPopupCount > 0) {
    return;
  }
  m_pointerInside =
      m_platform != nullptr && m_platform->hasPointerPosition() && m_platform->lastPointerSurface() == m_wlSurface;
  const bool ignorePointer = (m_activePanel != nullptr && m_activePanel->ignorePointerEvent());
  if (m_pointerInside) {
    if (!ignorePointer) {
      m_inputDispatcher.pointerEnter(
          static_cast<float>(m_platform->lastPointerX()), static_cast<float>(m_platform->lastPointerY()),
          m_platform->lastInputSerial()
      );
    }
  } else {
    if (!ignorePointer) {
      m_inputDispatcher.pointerLeave();
    }
  }
  requestRedraw();
}

std::optional<LayerPopupParentContext> PanelManager::popupParentContextForSurface(wl_surface* surface) const noexcept {
  if (surface == nullptr || surface != m_wlSurface) {
    return std::nullopt;
  }
  return fallbackPopupParentContext();
}

std::optional<LayerPopupParentContext> PanelManager::fallbackPopupParentContext() const noexcept {
  if (!isOpen() || m_surface == nullptr || m_wlSurface == nullptr || m_layerSurface == nullptr) {
    return std::nullopt;
  }

  LayerPopupParentContext context;
  context.surface = m_wlSurface;
  context.layerSurface = m_layerSurface->layerSurface();
  context.output = m_output;
  context.width = m_surface->width();
  context.height = m_surface->height();
  if (context.layerSurface == nullptr || context.width == 0 || context.height == 0) {
    return std::nullopt;
  }
  return context;
}

std::optional<LayerPopupParentContext>
PanelManager::popupParentContextForPanel(std::string_view panelId) const noexcept {
  if (m_persistentHost.hasPanel(panelId)) {
    return m_persistentHost.popupParentContext(panelId);
  }
  if (panelId != m_activePanelId) {
    return std::nullopt;
  }
  return fallbackPopupParentContext();
}

void PanelManager::onKeyboardEvent(const KeyboardEvent& event) {
  if (m_persistentHost.onKeyboardEvent(event)) {
    return;
  }
  // m_inTransition means the surface is still initializing.
  // Keyboard events during this window must be ignored.
  if (!isOpen() || m_inTransition) {
    return;
  }

  // Gate on compositor focus: route keys only when the surface owning this panel
  // input is the one the compositor reports as keyboard-focused.
  if (m_platform != nullptr) {
    wl_surface* const kbSurface = m_platform->lastKeyboardSurface();
    const bool onPanel = (m_wlSurface != nullptr && kbSurface == m_wlSurface);
    const bool onSelectPopup =
        (m_selectPopup != nullptr && m_selectPopup->isSelectDropdownOpen() && kbSurface == m_selectPopup->wlSurface());
    if (!onPanel && !onSelectPopup) {
      return;
    }
  }

  if (m_selectPopup != nullptr && m_selectPopup->isSelectDropdownOpen()) {
    m_selectPopup->onKeyboardEvent(event);
    return;
  }

  if (event.pressed && KeybindMatcher::matches(KeybindAction::Cancel, event.sym, event.modifiers)) {
    if (m_activePanel != nullptr
        && m_activePanel->handleGlobalKey(event.sym, event.modifiers, event.pressed, event.preedit)) {
      if (m_surface != nullptr && m_sceneRoot != nullptr && (m_sceneRoot->paintDirty() || m_sceneRoot->layoutDirty())) {
        if (m_sceneRoot->layoutDirty()) {
          m_surface->requestLayout();
        } else {
          m_surface->requestRedraw();
        }
      }
      return;
    }
    if (m_activePanel != nullptr && m_activePanel->dismissTransientUi()) {
      if (m_surface != nullptr && m_sceneRoot != nullptr && (m_sceneRoot->paintDirty() || m_sceneRoot->layoutDirty())) {
        if (m_sceneRoot->layoutDirty()) {
          m_surface->requestLayout();
        } else {
          m_surface->requestRedraw();
        }
      }
      return;
    }
    closePanel();
    return;
  }

  // A focused text input owns plain printable keys; the panel's global key
  // handler must not claim them (Space is a Validate chord but must type a space).
  const InputArea* const focusedArea = m_inputDispatcher.focusedArea();
  const bool textInputFocused = focusedArea != nullptr && focusedArea->textInputClient() != nullptr;
  const bool reserveForTextInput =
      event.pressed && textInputFocused && isPlainPrintableKey(event.utf32, event.modifiers, event.preedit);

  if (!reserveForTextInput
      && m_activePanel != nullptr
      && m_activePanel->handleGlobalKey(event.sym, event.modifiers, event.pressed, event.preedit)) {
    if (m_surface != nullptr && m_sceneRoot != nullptr && (m_sceneRoot->paintDirty() || m_sceneRoot->layoutDirty())) {
      if (m_sceneRoot->layoutDirty()) {
        m_surface->requestLayout();
      } else {
        m_surface->requestRedraw();
      }
    }
    return;
  }

  m_inputDispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
  if (m_surface != nullptr && m_sceneRoot != nullptr && (m_sceneRoot->paintDirty() || m_sceneRoot->layoutDirty())) {
    if (m_sceneRoot->layoutDirty()) {
      m_surface->requestLayout();
    } else {
      m_surface->requestRedraw();
    }
  }
}

void PanelManager::applyDetachedReveal(float progress) {
  m_detachedRevealProgress = std::clamp(progress, 0.0F, 1.0F);
  if (m_sceneRoot == nullptr) {
    if (m_surface != nullptr) {
      m_surface->clearBlurRegion();
    }
    return;
  }

  const float surfaceW = m_sceneRoot->width();
  const float surfaceH = m_sceneRoot->height();
  // The reveal covers the card and its shadow bleed. For a content-fitted panel that is a
  // band of the taller slot surface; otherwise it is the whole surface.
  const float spanY = m_contentFit.enabled ? static_cast<float>(m_panelInsetY - m_contentFit.slotInsetY) : 0.0F;
  const float spanH = m_contentFit.enabled
      ? static_cast<float>(
            m_contentFit.slotInsetY + static_cast<std::int32_t>(m_panelVisualHeight) + m_detachedBleedBottom
        )
      : surfaceH;
  const float clipX = 0.0F;
  const float clipY = spanY;
  const float clipW = surfaceW;
  const float clipH = spanH;

  if (m_detachedRevealClipNode != nullptr && m_detachedRevealContentNode != nullptr) {
    m_detachedRevealClipNode->setPosition(clipX, clipY);
    m_detachedRevealClipNode->setFrameSize(clipW, clipH);
    m_detachedRevealContentNode->setPosition(-clipX, -clipY);
    m_detachedRevealContentNode->setFrameSize(surfaceW, surfaceH);
  }

  if (m_detachedRevealClipNode != nullptr) {
    m_detachedRevealClipNode->setOpacity(m_detachedRevealProgress);
  }
  const bool blurVisible = m_detachedRevealProgress >= kPanelBlurFromProgress;
  applyPanelCompositorBlur(
      m_panelInsetX, m_panelInsetY, static_cast<int>(m_panelVisualWidth), static_cast<int>(m_panelVisualHeight),
      static_cast<int>(std::lround(clipX)), static_cast<int>(std::lround(clipY)),
      blurVisible ? static_cast<int>(std::lround(clipW)) : 0, blurVisible ? static_cast<int>(std::lround(clipH)) : 0
  );
}

void PanelManager::applyPanelCompositorBlur(
    int bodyX, int bodyY, int bodyW, int bodyH, int clipX, int clipY, int clipW, int clipH
) {
  // The blur region is compositor surface state, not a scene node. Callers pass the
  // same body and clip rectangles used by the reveal animation so protocol state
  // cannot get ahead of scene rendering.
  if (m_surface == nullptr || m_activePanel == nullptr) {
    return;
  }

  if (blurTraceEnabled()) {
    kLog.debug(
        "blur-trace panel-blur-input mode={} progress={:.3F} phase={} surface={}x{} body={}:{}+{}x{} "
        "clip={}:{}+{}x{}",
        "detached",
        m_detachedRevealProgress, uiPhaseName(currentUiPhase()),
        m_surface->width(), m_surface->height(), bodyX, bodyY, bodyW, bodyH, clipX, clipY, clipW, clipH
    );
  }

  if (bodyW <= 0 || bodyH <= 0 || clipW <= 0 || clipH <= 0) {
    if (blurTraceEnabled()) {
      kLog.debug("blur-trace panel-blur-clear reason=empty-input");
    }
    m_surface->clearBlurRegion();
    return;
  }

  const float radius = Style::scaledRadiusXl(m_activePanel->contentScale());
  const CornerShapes corners = CornerShapes{};
  const RectInsets logicalInset =
      RectInsets{};
  const Radii radii = Radii{radius, radius, radius, radius};
  auto strips = Surface::tessellateShape(bodyX, bodyY, bodyW, bodyH, corners, logicalInset, radii);
  if (strips.empty()) {
    if (blurTraceEnabled()) {
      kLog.debug("blur-trace panel-blur-clear reason=empty-shape");
    }
    m_surface->clearBlurRegion();
    return;
  }

  const int clipRight = clipX + clipW;
  const int clipBottom = clipY + clipH;
  std::vector<InputRect> clipped;
  clipped.reserve(strips.size());
  for (const auto& s : strips) {
    const int sxLeft = std::max(s.x, clipX);
    const int sxRight = std::min(s.x + s.width, clipRight);
    const int syTop = std::max(s.y, clipY);
    const int syBottom = std::min(s.y + s.height, clipBottom);
    if (sxRight > sxLeft && syBottom > syTop) {
      clipped.push_back({sxLeft, syTop, sxRight - sxLeft, syBottom - syTop});
    }
  }

  if (clipped.empty()) {
    if (blurTraceEnabled()) {
      kLog.debug("blur-trace panel-blur-clear reason=empty-clipped");
    }
    m_surface->clearBlurRegion();
    return;
  }

  if (blurTraceEnabled()) {
    const auto bounds = boundsForPanelTrace(clipped);
    kLog.debug(
        "blur-trace panel-blur-set mode={} progress={:.3F} rects={} bounds={}:{}+{}x{}",
        "detached",
        m_detachedRevealProgress, clipped.size(), bounds.x, bounds.y,
        bounds.width, bounds.height
    );
  }
  m_surface->setBlurRegion(clipped);
}

void PanelManager::onConfigReloaded() {
  m_persistentHost.onConfigReloaded();
  if (!isOpen() || m_config == nullptr || m_activePanel == nullptr) {
    return;
  }
  if (m_layerSurface != nullptr) {
    const LayerShellLayer panelLayer = resolveFloatingPanelLayer(m_config, m_activePanelId, *m_activePanel);
    if (panelLayer != m_panelLayer) {
      m_clickShield.setLayer(panelLayer);
      m_layerSurface->setLayer(panelLayer);
      m_panelLayer = panelLayer;
    }
  }

  applyDetachedReveal(m_detachedRevealProgress);
  const float panelBackgroundOpacity =
      shell::panel_surface::backgroundOpacity(m_config);
  m_activePanel->setPanelCardOpacity(shell::panel_surface::cardOpacity(m_config, panelBackgroundOpacity));
  if (m_bgNode != nullptr) {
    auto* bg = static_cast<Box*>(m_bgNode);
    bg->setPanelStyle(m_config->config().shell.panel.borders);
    bg->setFill(colorSpecFromRole(ColorRole::Surface, panelBackgroundOpacity));
    if (m_config->config().shell.panel.borders) {
      bg->setBorder(
          shell::panel_surface::borderColor(m_config, panelBackgroundOpacity),
          shell::panel_surface::borderWidth(m_config)
      );
    }
  }
  if (m_panelShadowNode != nullptr) {
    const auto& shadowConfig = m_config->config().shell.shadow;
    const bool panelShadow =
        m_config->config().shell.panel.shadow && shell::surface_shadow::enabled(true, shadowConfig);
    m_panelShadowNode->setVisible(panelShadow);
    if (panelShadow) {
      const float shadowRadius = Style::scaledRadiusXl(m_activePanel->contentScale());
      m_panelShadowNode->setStyle(
          shell::surface_shadow::style(
              shadowConfig, panelBackgroundOpacity,
              shell::surface_shadow::Shape{.radius = Radii{shadowRadius, shadowRadius, shadowRadius, shadowRadius}}
          )
      );
    }
  }
  if (m_surface != nullptr) {
    m_surface->requestUpdate();
  }
}

void PanelManager::buildScene(std::uint32_t width, std::uint32_t height) {
  uiAssertNotRendering("PanelManager::buildScene");
  if (m_renderContext == nullptr || m_activePanel == nullptr) {
    return;
  }
  Renderer& renderer = m_surface->renderTarget().renderer();
  const bool hasDecoration = m_activePanel->hasDecoration();

  const auto w = static_cast<float>(width);
  const auto h = static_cast<float>(height);

  if (m_sceneRoot == nullptr) {
    m_sceneRoot = ui::node({});
    m_sceneRoot->setAnimationManager(&m_animations);
    if (m_layerSurface != nullptr && m_renderContext != nullptr) {
      m_selectPopup = std::make_unique<SelectDropdownPopup>(m_platform->wayland(), *m_renderContext);
      if (m_config != nullptr) {
        m_selectPopup->setShadowConfig(m_config->config().shell.shadow);
      }
      m_selectPopup->setParent(m_layerSurface->layerSurface(), m_wlSurface, m_output);
      m_sceneRoot->setPopupContext(m_selectPopup.get());
    }
    m_sceneRoot->setSize(w, h);

    Node* sceneParent = m_sceneRoot.get();
    auto revealClip = ui::node({});
    revealClip->setClipChildren(true);
    m_detachedRevealClipNode = m_sceneRoot->addChild(std::move(revealClip));

    auto revealContent = ui::node({});
    m_detachedRevealContentNode = m_detachedRevealClipNode->addChild(std::move(revealContent));
    sceneParent = m_detachedRevealContentNode;

    if (hasDecoration && m_config != nullptr && shell::surface_shadow::enabled(true, m_config->config().shell.shadow)) {
      auto shadow = ui::box({});
      m_panelShadowNode = static_cast<Box*>(sceneParent->addChild(std::move(shadow)));
      m_panelShadowNode->setZIndex(-1);
      m_panelShadowNode->setVisible(m_config->config().shell.panel.shadow);
    }

    if (hasDecoration) {
      auto bg = ui::box({});
      const bool panelBorders = m_config != nullptr && m_config->config().shell.panel.borders;
      bg->setPanelStyle(panelBorders);
      const float backgroundOpacity = shell::panel_surface::backgroundOpacity(m_config);
      bg->setFill(colorSpecFromRole(ColorRole::Surface, backgroundOpacity));
      if (panelBorders) {
        bg->setBorder(
            shell::panel_surface::borderColor(m_config, backgroundOpacity),
            shell::panel_surface::borderWidth(m_config)
        );
      }
      m_bgNode = sceneParent->addChild(std::move(bg));
    }


    // Create panel content inside a wrapper node for staggered fade-in
    auto contentWrapper = ui::node({});
    m_contentNode = contentWrapper.get();
    m_activePanel->setAnimationManager(&m_animations);
    const float panelBackgroundOpacity =
        shell::panel_surface::backgroundOpacity(m_config);
    m_activePanel->setPanelCardOpacity(shell::panel_surface::cardOpacity(m_config, panelBackgroundOpacity));
    m_activePanel->create();
    m_activePanel->onOpen(m_pendingOpenContext);
    m_pendingOpenContext.clear();
    if (m_activePanel->root() != nullptr) {
      contentWrapper->addChild(m_activePanel->releaseRoot());
    }
    sceneParent->addChild(std::move(contentWrapper));

    m_inputDispatcher.setSceneRoot(m_sceneRoot.get());
    m_inputDispatcher.setTextInputContext(m_wlSurface, m_platform->wayland().textInputService());
    m_inputDispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
      m_platform->setCursorShape(serial, shape);
    });
    m_inputDispatcher.setHoverChangeCallback([this](InputArea* /*old*/, InputArea* next) {
      if (m_layerSurface != nullptr) {
        TooltipManager::instance().onHoverChange(next, m_layerSurface->layerSurface(), m_output);
      }
    });
    m_inputDispatcher.setFocusChangeCallback([this](InputArea* /*old*/, InputArea* next) {
      if (m_activePanel != nullptr && next != nullptr) {
        m_activePanel->scrollFocusedInputIntoView(next);
      }
    });

    applyDetachedReveal(0.0F);
    m_animations.animate(
        0.0F, 1.0F, kPanelFadeInMs, Easing::EaseOutCubic, [this](float v) { applyDetachedReveal(v); }, {},
        m_sceneRoot.get()
    );

    m_surface->setSceneRoot(m_sceneRoot.get());

    // Set initial keyboard focus if the panel requests it
    if (m_activePanel != nullptr) {
      if (auto* focusArea = m_activePanel->initialFocusArea(); focusArea != nullptr) {
        m_inputDispatcher.setFocus(focusArea);
      }
    }
  }

  m_sceneRoot->setSize(w, h);
  if (m_detachedRevealContentNode != nullptr) {
    m_detachedRevealContentNode->setFrameSize(w, h);
  }

  // Honor the compositor-configured surface size: a filled axis derives its
  // visual size from the configure (surface minus shadow bleed), and a fixed
  // axis the compositor configured smaller than requested lays out at the
  // configured size instead of overflowing the buffer.
  const std::int32_t availW = static_cast<std::int32_t>(width) - m_panelInsetX - m_detachedBleedRight;
  const std::int32_t availH = static_cast<std::int32_t>(height) - m_panelInsetY - m_detachedBleedBottom;
  if (availW > 0 && (m_panelFillWidth || m_panelVisualWidth > static_cast<std::uint32_t>(availW))) {
    m_panelVisualWidth = static_cast<std::uint32_t>(availW);
  }
  if (m_contentFit.enabled) {
    // The card fits inside the slot; a smaller configure shrinks the slot instead.
    const std::int32_t slotAvailH =
        static_cast<std::int32_t>(height) - m_contentFit.slotInsetY - m_detachedBleedBottom;
    if (slotAvailH > 0 && m_contentFit.slotHeight > static_cast<std::uint32_t>(slotAvailH)) {
      m_contentFit.slotHeight = static_cast<std::uint32_t>(slotAvailH);
    }
  } else if (availH > 0 && (m_panelFillHeight || m_panelVisualHeight > static_cast<std::uint32_t>(availH))) {
    m_panelVisualHeight = static_cast<std::uint32_t>(availH);
  }
  if (m_surface != nullptr) {
    m_surface->setInputRegion({InputRect{
        m_panelInsetX, m_panelInsetY, static_cast<int>(m_panelVisualWidth), static_cast<int>(m_panelVisualHeight)
    }});
  }

  applyDetachedReveal(m_detachedRevealProgress);

  placePanelChrome();
  {
    UiPhaseScope updatePhase(UiPhase::Update);
    m_activePanel->update(renderer);
  }
  {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    layoutActivePanel(renderer);
  }
  applyPendingPanelFocus();
  if (m_pointerInside && (m_activePanel == nullptr || !m_activePanel->ignorePointerEvent())) {
    m_inputDispatcher.syncPointerHover();
  }
}

void PanelManager::placePanelChrome() {
  if (m_activePanel == nullptr || m_sceneRoot == nullptr) {
    return;
  }
  const float w = m_sceneRoot->width();
  const float h = m_sceneRoot->height();
  const auto panelX = static_cast<float>(m_panelInsetX);
  const auto panelY = static_cast<float>(m_panelInsetY);
  const float panelW = m_panelVisualWidth > 0 ? static_cast<float>(m_panelVisualWidth) : w;
  const float panelH = m_panelVisualHeight > 0 ? static_cast<float>(m_panelVisualHeight) : h;
  const float bgX = panelX;
  const float bgY = panelY;
  const float bgW = panelW;
  const float bgH = panelH;

  if (m_panelShadowNode != nullptr && m_config != nullptr) {
    const auto& shadowConfig = m_config->config().shell.shadow;
    const bool panelShadow =
        m_config->config().shell.panel.shadow && shell::surface_shadow::enabled(true, shadowConfig);
    m_panelShadowNode->setVisible(panelShadow);
    const auto shadowOff = shadowDirectionOffset(shadowConfig.direction);
    const auto shadowOffsetX = static_cast<float>(shadowOff.x);
    const auto shadowOffsetY = static_cast<float>(shadowOff.y);
    m_panelShadowNode->setPosition(bgX + shadowOffsetX, bgY + shadowOffsetY);
    m_panelShadowNode->setSize(bgW, bgH);
    if (panelShadow) {
      const float shadowRadius = Style::scaledRadiusXl(m_activePanel->contentScale());
      const float panelBackgroundOpacity = shell::panel_surface::backgroundOpacity(m_config);
      m_panelShadowNode->setStyle(
          shell::surface_shadow::style(
              shadowConfig, panelBackgroundOpacity,
              shell::surface_shadow::Shape{.radius = Radii{shadowRadius, shadowRadius, shadowRadius, shadowRadius}}
          )
      );
    }
  }

  if (m_bgNode != nullptr) {
    m_bgNode->setPosition(bgX, bgY);
    m_bgNode->setSize(bgW, bgH);
  }

  // Re-apply opacity-dependent styling for bg, shadow, and contact-shadow.
  // Ensures these stay in sync if the bar config changed.

  const float kPadding = m_activePanel->hasDecoration() ? m_activePanel->contentScale() * Style::panelPadding : 0.0F;
  m_contentWidth = panelW - kPadding * 2.0F;
  // A content-fitted panel lays its content out at the target height once; while the card
  // animates toward it, the content node clips to the card instead of relaying out.
  const bool fitted = m_contentFit.enabled && m_contentFit.targetHeight > 0;
  m_contentHeight = std::max(0.0F, (fitted ? static_cast<float>(m_contentFit.targetHeight) : panelH) - kPadding * 2.0F);
  if (m_contentNode != nullptr) {
    m_contentNode->setClipChildren(m_contentFit.enabled);
    m_contentNode->setPosition(panelX + kPadding, panelY + kPadding);
    m_contentNode->setSize(m_contentWidth, std::max(0.0F, panelH - kPadding * 2.0F));
  }
}

void PanelManager::layoutActivePanel(Renderer& renderer) {
  m_activePanel->layout(renderer, m_contentWidth, m_contentHeight);
  if (!m_contentFit.enabled) {
    return;
  }
  const float padding = m_activePanel->hasDecoration() ? m_activePanel->contentScale() * Style::panelPadding : 0.0F;
  const float natural = m_activePanel->measureContentHeight(renderer, m_contentWidth);
  const auto target = std::clamp<std::uint32_t>(
      static_cast<std::uint32_t>(std::ceil(std::max(0.0F, natural) + padding * 2.0F)), 1U,
      std::max(1U, m_contentFit.slotHeight)
  );
  if (target != m_contentFit.targetHeight) {
    const bool firstMeasure = m_contentFit.targetHeight == 0;
    m_contentFit.targetHeight = target;
    if (m_contentFit.animation != 0) {
      m_animations.cancel(m_contentFit.animation);
      m_contentFit.animation = 0;
    }
    if (firstMeasure || m_panelVisualHeight == target) {
      // The open reveal already animates the first appearance.
      (void)setCardHeight(target);
    } else {
      m_contentFit.animation = m_animations.animate(
          static_cast<float>(m_panelVisualHeight), static_cast<float>(target), Style::animNormal, Easing::EaseOutCubic,
          [this](float height) {
            if (setCardHeight(static_cast<std::uint32_t>(std::lround(height)))) {
              placePanelChrome();
            }
          },
          [this] { m_contentFit.animation = 0; }, m_sceneRoot.get()
      );
    }
    placePanelChrome();
  }
  // Measuring left the content at its natural height; lay it out at the target.
  m_activePanel->layout(renderer, m_contentWidth, m_contentHeight);
}

bool PanelManager::setCardHeight(std::uint32_t height) {
  height = std::clamp<std::uint32_t>(height, 1U, std::max(1U, m_contentFit.slotHeight));
  if (height == m_panelVisualHeight) {
    return false;
  }
  const std::uint32_t slack = m_contentFit.slotHeight - height;
  std::int32_t offset = 0;
  if (m_contentFit.align == CardAlign::End) {
    offset = static_cast<std::int32_t>(slack);
  } else if (m_contentFit.align == CardAlign::Center) {
    offset = static_cast<std::int32_t>(slack / 2U);
  }
  m_panelVisualHeight = height;
  m_panelInsetY = m_contentFit.slotInsetY + offset;

  // Everything outside the scene that describes the panel's body follows the card.
  if (m_surface != nullptr) {
    m_surface->setInputRegion(
        {InputRect{m_panelInsetX, m_panelInsetY, static_cast<int>(m_panelVisualWidth), static_cast<int>(height)}}
    );
  }
  if (m_panelOutputInputRect.has_value()) {
    m_panelOutputInputRect->y = m_contentFit.slotOutputY + offset;
    m_panelOutputInputRect->height = static_cast<int>(height);
    if (m_output != nullptr) {
      m_clickShield.setPanelInputRect(m_output, *m_panelOutputInputRect);
    }
  }
  // The reveal function republishes the compositor blur region.
  applyDetachedReveal(m_detachedRevealProgress);
  return true;
}

void PanelManager::applyPendingPanelFocus() {
  if (m_activePanel == nullptr) {
    return;
  }
  if (auto* area = m_activePanel->takePendingFocusArea(); area != nullptr) {
    m_inputDispatcher.setFocus(area);
  }
}

void PanelManager::prepareFrame(bool needsUpdate, bool needsLayout) {
  if (m_renderContext == nullptr || m_surface == nullptr) {
    return;
  }
  if (m_activePanel == nullptr) {
    return;
  }

  m_renderContext->makeCurrent(m_surface->renderTarget());
  Renderer& renderer = m_surface->renderTarget().renderer();

  const auto width = m_surface->width();
  const auto height = m_surface->height();

  const bool needsSceneBuild = m_sceneRoot == nullptr
      || static_cast<std::uint32_t>(std::round(m_sceneRoot->width())) != width
      || static_cast<std::uint32_t>(std::round(m_sceneRoot->height())) != height;
  if (needsSceneBuild) {
    buildScene(width, height);
  }

  if (!needsSceneBuild && needsUpdate) {
    UiPhaseScope updatePhase(UiPhase::Update);
    m_activePanel->update(renderer);
  }
  if (!needsSceneBuild && needsLayout) {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    if (m_activePanel != nullptr) {
      layoutActivePanel(renderer);
    }
    if (m_pointerInside && (m_activePanel == nullptr || !m_activePanel->ignorePointerEvent())) {
      m_inputDispatcher.syncPointerHover();
    }
  }
  if (!needsSceneBuild && (needsUpdate || needsLayout)) {
    applyPendingPanelFocus();
  }
}

std::vector<std::string> PanelManager::availablePanelIds() const {
  std::vector<std::string> ids;
  ids.reserve(m_panels.size());
  for (const auto& entry : m_panels) {
    ids.push_back(entry.first);
  }
  m_persistentHost.appendPanelIds(ids);
  return ids;
}

void PanelManager::registerIpc(IpcService& ipc) {
  auto parseOpenArgs = [](std::string_view rawArgs, std::string_view command, std::string& panelId,
                          std::string& context) -> std::optional<std::string> {
    const std::string_view args = StringUtils::trimLeftView(rawArgs);
    if (args.empty()) {
      return "error: " + std::string(command) + " requires a panel id\n";
    }

    const auto sep = args.find_first_of(" \t\n\r\f\v");
    if (sep == std::string_view::npos) {
      panelId = std::string(args);
      context.clear();
      return std::nullopt;
    }

    panelId = std::string(args.substr(0, sep));
    // Preserve the context verbatim (only strip the separator's leading
    // whitespace) — trailing whitespace can be significant, e.g. a command.
    context = std::string(StringUtils::trimLeftView(args.substr(sep + 1)));
    return std::nullopt;
  };

  auto unknownPanelError = [this](std::string_view panelId) -> std::string {
    std::vector<std::string> ids = availablePanelIds();
    std::ranges::sort(ids);

    std::string error = "error: unknown panel \"" + std::string(panelId) + "\"";
    if (!ids.empty()) {
      error += " (available: " + StringUtils::join(ids, ", ") + ")";
    }
    error += '\n';
    return error;
  };

  ipc.bind(
      okaru::cli::msg::panelToggle,
      [this, parseOpenArgs, unknownPanelError](const std::string& args) -> std::string {
        std::string panelId;
        std::string context;
        if (auto error = parseOpenArgs(args, "panel-toggle", panelId, context)) {
          return *error;
        }
        if (!m_panels.contains(panelId) && !m_persistentHost.hasPanel(panelId)) {
          return unknownPanelError(panelId);
        }
        // Output left unset: openPanel resolves it (focus source, else compositor probe).
        if (context.empty()) {
          togglePanel(panelId);
        } else {
          togglePanel(panelId, PanelOpenRequest{.context = context});
        }
        return "ok\n";
      }
  );

  ipc.bind(
      okaru::cli::msg::panelOpen, [this, parseOpenArgs, unknownPanelError](const std::string& args) -> std::string {
        std::string panelId;
        std::string context;
        if (auto error = parseOpenArgs(args, "panel-open", panelId, context)) {
          return *error;
        }
        if (!m_panels.contains(panelId) && !m_persistentHost.hasPanel(panelId)) {
          return unknownPanelError(panelId);
        }

        if (isOpen() && !m_closing && m_activePanelId == panelId) {
          if (!context.empty() && m_activePanel != nullptr) {
            m_activePanel->onOpen(context);
            refresh();
          }
          return "ok\n";
        }

        // Output left unset: openPanel resolves it (focus source, else compositor probe).
        openPanel(panelId, PanelOpenRequest{.context = context});
        return "ok\n";
      }
  );

  ipc.bind(okaru::cli::msg::panelClose, [this, unknownPanelError](const std::string& args) -> std::string {
    const std::string panelId = StringUtils::trim(args);
    if (!panelId.empty() && StringUtils::splitWhitespace(panelId).size() != 1) {
      return "error: panel-close accepts at most one panel id\n";
    }
    if (!panelId.empty() && !m_panels.contains(panelId) && !m_persistentHost.hasPanel(panelId)) {
      return unknownPanelError(panelId);
    }

    if (!panelId.empty() && m_persistentHost.hasPanel(panelId)) {
      m_persistentHost.close(panelId);
      return "ok\n";
    }
    if (panelId.empty() || isOpenPanel(panelId)) {
      closePanel();
    }
    return "ok\n";
  });

  const auto rejectSettingsArgs = [](const std::string& args, std::string_view command) -> std::optional<std::string> {
    if (StringUtils::trim(args).empty()) {
      return std::nullopt;
    }
    return std::format("error: {} accepts no arguments\n", command);
  };

  ipc.bind(okaru::cli::msg::settingsOpen, [this](const std::string& args) -> std::string {
    openSettingsWindow(std::string(StringUtils::trimLeftView(args)));
    return "ok\n";
  });

  ipc.bind(okaru::cli::msg::settingsOpenWidget, [this, &ipc](const std::string& args) -> std::string {
    const auto parts = okaru::ipc::splitWords(args);
    std::string barName;
    std::string widgetName;
    if (parts.size() == 2) {
      barName = parts[0];
      widgetName = parts[1];
    } else if (parts.empty()) {
      // Invoked from a bar widget gesture: the widget is the implicit target.
      const auto& context = ipc.invocationContext();
      if (!context.has_value() || context->widgetName.empty()) {
        return "error: settings-open-widget needs <bar-name> <widget-name> unless invoked from a bar widget\n";
      }
      barName = context->barName;
      widgetName = context->widgetName;
    } else {
      return "error: settings-open-widget takes either no arguments or <bar-name> <widget-name>\n";
    }

    if (!m_openWidgetSettings) {
      return "error: settings window unavailable\n";
    }
    if (isOpen()) {
      closePanel();
    }
    m_openWidgetSettings(std::move(barName), std::move(widgetName));
    return "ok\n";
  });

  ipc.bind(okaru::cli::msg::settingsOpenPlugin, [this](const std::string& args) -> std::string {
    const auto parts = okaru::ipc::splitWords(args);
    if (parts.size() != 1) {
      return "error: settings-open-plugin takes <plugin-id> (e.g. okaru/notes)\n";
    }
    const std::string& pluginId = parts[0];
    if (!scripting::isValidPluginId(pluginId)) {
      return std::format("error: \"{}\" is not a plugin id (expected author/plugin)\n", pluginId);
    }

    if (!m_openPluginSettings) {
      return "error: settings window unavailable\n";
    }
    if (!openPluginSettings(pluginId)) {
      return std::format("error: plugin \"{}\" is not enabled or has no settings\n", pluginId);
    }
    return "ok\n";
  });

  ipc.bind(okaru::cli::msg::settingsClose, [this, rejectSettingsArgs](const std::string& args) -> std::string {
    if (auto error = rejectSettingsArgs(args, "settings-close")) {
      return *error;
    }
    closeSettingsWindow();
    return "ok\n";
  });

  ipc.bind(okaru::cli::msg::settingsToggle, [this](const std::string& args) -> std::string {
    toggleSettingsWindow(std::string(StringUtils::trimLeftView(args)));
    return "ok\n";
  });
}
