#include "shell/lockscreen/lock_surface.h"

#include "capture/screencopy_capture.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "ext-session-lock-v1-client-protocol.h"
#include "i18n/i18n.h"
#include "render/core/blur_cache.h"
#include "render/core/render_styles.h"
#include "render/core/shared_texture_cache.h"
#include "render/render_context.h"
#include "render/scene/wallpaper_node.h"
#include "shell/lockscreen/lockscreen_session_actions.h"
#include "shell/session/session_action_meta.h"
#include "shell/session/session_action_runner.h"
#include "ui/builders.h"
#include "ui/controls/lockscreen_transition_cover.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <memory>
#include <string_view>
#include <utility>
#include <wayland-client-core.h>

namespace {

  constexpr Logger kLog("lockscreen");

  const ext_session_lock_surface_v1_listener kLockSurfaceListener = {
      .configure = &LockSurface::handleConfigure,
  };

  bool parseColorWallpaperPath(std::string_view path, Color& out) {
    constexpr std::string_view kPrefix = "color:";
    if (!path.starts_with(kPrefix)) {
      return false;
    }
    return tryParseHexColor(path.substr(kPrefix.size()), out);
  }

} // namespace

LockSurface::LockSurface(WaylandConnection& connection, ConfigService* config) : Surface(connection), m_config(config) {
  {
    auto backgroundLayer = ui::node({});
    backgroundLayer->setZIndex(0);
    m_backgroundLayer = m_root.addChild(std::move(backgroundLayer));
  }

  auto wallpaper = std::make_unique<WallpaperNode>();
  m_wallpaper = static_cast<WallpaperNode*>(m_backgroundLayer->addChild(std::move(wallpaper)));
  m_wallpaper->setZIndex(0);

  m_backgroundLayer->addChild(
      ui::box({
          .out = &m_backdrop,
          .configure = [](Box& box) { box.setZIndex(-1); },
      })
  );

  m_okaru.build(m_root);

  m_root.addChild(
      ui::input({
          .out = &m_passwordField,
          .passwordMode = true,
          .onChange =
              [this](const std::string& value) {
                if (m_onPasswordChanged) {
                  m_onPasswordChanged(value);
                }
              },
          .onSubmit =
              [this](const std::string& /*value*/) {
                if (m_onLogin) {
                  m_onLogin();
                }
              },
      })
  );
  m_okaru.adoptPasswordField(m_passwordField);

  {
    auto transitionCover = std::make_unique<LockscreenTransitionCover>();
    transitionCover->setVisible(false);
    transitionCover->setZIndex(1000);
    m_transitionCover = static_cast<LockscreenTransitionCover*>(m_root.addChild(std::move(transitionCover)));
  }

  m_inputDispatcher.setSceneRoot(&m_root);
  m_inputDispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
    m_connection.setCursorShape(serial, shape);
  });

  setSceneRoot(&m_root);
  setAnimationManager(&m_animations);
  m_root.setAnimationManager(&m_animations);
  setConfigureCallback([this](std::uint32_t /*width*/, std::uint32_t /*height*/) { requestLayout(); });
  setPrepareFrameCallback([this](bool needsUpdate, bool needsLayout) { prepareFrame(needsUpdate, needsLayout); });
  requestUpdate();
}

LockSurface::~LockSurface() {
  invalidateSleepFrame();
  releaseCaptureTextures();
  m_connection.unregisterSurface(m_surface);
  if (m_lockSurface != nullptr) {
    // If get_lock_surface raced with the output disappearing server-side, some
    // compositors (e.g. Hyprland) silently drop the request without binding the
    // new object id. The compositor otherwise sends the first configure event
    // immediately, so "output gone and no configure ever received" identifies
    // such a zombie proxy: sending its destructor request would be answered
    // with a fatal invalid-object protocol error. Destroy it client-side only.
    const bool outputGone = m_connection.findOutputByWl(m_output) == nullptr;
    if (!m_receivedConfigure && outputGone) {
      wl_proxy_destroy(reinterpret_cast<wl_proxy*>(m_lockSurface));
    } else {
      ext_session_lock_surface_v1_destroy(m_lockSurface);
    }
    m_lockSurface = nullptr;
  }
}

void LockSurface::setSessionActions(SessionActionRunner* sessionActions) {
  m_sessionActions = sessionActions;
  requestUpdate();
}

bool LockSurface::initialize(ext_session_lock_v1* lock, wl_output* output, std::int32_t scale) {
  if (lock == nullptr || output == nullptr || renderContext() == nullptr) {
    return false;
  }

  if (const auto* outputInfo = m_connection.findOutputByWl(output); outputInfo != nullptr) {
    setConfiguredScaleNumerator(
        outputInfo->configuredScaleNumerator > 0 ? static_cast<std::uint32_t>(outputInfo->configuredScaleNumerator) : 1U
    );
    setBufferScale(outputInfo->scale);
  } else {
    setBufferScale(scale);
  }

  // Upload the immutable cover before the lock request is flushed. The first
  // configured lock-surface frame can then be the captured desktop immediately.
  if (m_transitionPhase == TransitionPhase::Cover && !ensureTransitionCaptureTexture()) {
    m_transitionPhase = TransitionPhase::Disabled;
    m_transitionProgress = 1.0F;
    syncTransitionCover();
  }

  if (!createWlSurface()) {
    return false;
  }

  // Keep the lock surface out of text-input-v3; it can leave Chromium's fcitx
  // context inactive after unlock. Password input still uses wl_keyboard.

  m_output = output;
  m_connection.registerSurfaceOutput(m_surface, output);
  m_lockSurface = ext_session_lock_v1_get_lock_surface(lock, m_surface, output);
  if (m_lockSurface == nullptr) {
    destroySurface();
    return false;
  }

  if (ext_session_lock_surface_v1_add_listener(m_lockSurface, &kLockSurfaceListener, this) != 0) {
    ext_session_lock_surface_v1_destroy(m_lockSurface);
    m_lockSurface = nullptr;
    destroySurface();
    return false;
  }

  setRunning(true);
  return true;
}

void LockSurface::syncOutputScale(std::int32_t bufferScale, std::uint32_t configuredScaleNumerator) {
  updateOutputScale(bufferScale, configuredScaleNumerator);
}

void LockSurface::setLockedState(bool locked) {
  if (m_locked == locked) {
    return;
  }
  m_locked = locked;
  if (m_locked) {
    // Never carry a revealed password across lock cycles.
    if (m_passwordField != nullptr) {
      m_passwordField->setPasswordRevealed(false);
    }
    focusPasswordField();
  } else {
    m_inputDispatcher.setFocus(nullptr);
  }
  requestUpdate();
}

bool LockSurface::passwordFieldContainsPoint(float sceneX, float sceneY) const {
  return m_passwordField != nullptr && m_passwordField->containsScenePoint(sceneX, sceneY);
}

void LockSurface::focusPasswordField() {
  if (!m_locked || m_blackout || m_passwordField == nullptr) {
    return;
  }
  m_inputDispatcher.setFocus(m_passwordField->inputArea());
}

void LockSurface::setPromptState(
    std::string user, std::string password, std::string status, bool error, bool authenticating
) {
  if (m_user == user
      && m_password == password
      && m_status == status
      && m_error == error
      && m_authenticating == authenticating) {
    return;
  }
  m_user = std::move(user);
  m_password = std::move(password);
  m_status = std::move(status);
  m_error = error;
  m_authenticating = authenticating;
  requestUpdate();
}

void LockSurface::setCapsLock(bool capsLock) {
  if (m_capsLock == capsLock) {
    return;
  }
  m_capsLock = capsLock;
  requestUpdate();
}

void LockSurface::setWallpaperPath(std::string wallpaperPath) {
  if (m_wallpaperPath == wallpaperPath) {
    return;
  }

  if (m_blurredWallpaperTexture.id != 0 && renderContext() != nullptr) {
    renderContext()->backend().makeCurrentNoSurface();
    renderContext()->textureManager().unload(m_blurredWallpaperTexture);
    m_blurredWallpaperTexture = {};
  }

  // Keep the current wallpaper visible until applyWallpaperTexture() loads the new path.
  m_wallpaperPath = std::move(wallpaperPath);
  m_wallpaperDirty = true;
  requestLayout();
}

void LockSurface::setWallpaperFillMode(WallpaperFillMode fillMode) {
  if (m_wallpaperFillMode == fillMode) {
    return;
  }
  m_wallpaperFillMode = fillMode;
  if (m_wallpaper != nullptr) {
    m_wallpaper->setFillMode(m_wallpaperFillMode);
  }
  requestRedraw();
}

void LockSurface::setWallpaperCoverage(TextureCoverage coverage) {
  if (m_wallpaperCoverage == coverage) {
    return;
  }
  m_wallpaperCoverage = coverage;
  m_wallpaperDirty = true;
  requestLayout();
}

void LockSurface::setWallpaperFillColor(Color fillColor) {
  if (m_wallpaperFillColor == fillColor) {
    return;
  }
  m_wallpaperFillColor = fillColor;
  if (m_wallpaper != nullptr) {
    m_wallpaper->setFillColor(m_wallpaperFillColor);
  }
  if (m_backdrop != nullptr) {
    m_backdrop->setVisible(m_wallpaperFillColor.a > 0.0F);
    m_backdrop->setStyle(
        RoundedRectStyle{
            .fill = m_wallpaperFillColor,
            .fillMode = FillMode::Solid,
        }
    );
  }
  requestRedraw();
}

void LockSurface::setDesktopCapture(std::optional<ScreencopyImage> capture, bool useAsBackground) {
  m_desktopCapture = std::move(capture);
  m_useDesktopCaptureBackground = useAsBackground && m_desktopCapture.has_value() && !m_desktopCapture->rgba.empty();
  m_captureDirty = true;
  releaseCaptureTextures();
  requestLayout();
}

bool LockSurface::usesDesktopCaptureBackground() const noexcept {
  return m_useDesktopCaptureBackground && m_desktopCapture.has_value() && !m_desktopCapture->rgba.empty();
}

void LockSurface::configureTransition(
    std::optional<LockscreenTransitionKind> transition, const LockscreenTransitionParams& params, float durationMs
) {
  invalidateSleepFrame();
  cancelTransitionAnimation();
  m_transitionParams = params;
  m_transitionDurationMs = std::max(0.0F, durationMs);
  m_transitionProgress = 1.0F;
  m_enterTransitionRequested = false;

  if (!transition.has_value() || m_blackout || !m_desktopCapture.has_value() || m_desktopCapture->rgba.empty()) {
    m_transitionPhase = TransitionPhase::Disabled;
    syncTransitionCover();
    return;
  }

  m_transition = *transition;
  m_transitionProgress = 0.0F;
  m_transitionPhase = TransitionPhase::Cover;
  syncTransitionCover();
  requestLayout();
}

void LockSurface::startEnterTransition() {
  m_enterTransitionRequested = true;
  if (m_transitionPhase == TransitionPhase::Ready) {
    beginEnterAnimation();
  }
}

void LockSurface::startExitTransition() {
  invalidateSleepFrame();
  m_enterTransitionRequested = false;
  if (m_transitionPhase == TransitionPhase::ExitComplete
      || m_transitionPhase == TransitionPhase::ExitEndpoint
      || m_transitionPhase == TransitionPhase::Exiting) {
    return;
  }

  if (m_transitionPhase != TransitionPhase::Disabled && m_transitionCaptureTexture.id == 0) {
    (void)ensureTransitionCaptureTexture();
  }
  if (m_transitionPhase == TransitionPhase::Disabled || m_transitionCaptureTexture.id == 0) {
    m_transitionPhase = TransitionPhase::ExitComplete;
    notifyTransitionStateChanged();
    return;
  }

  cancelTransitionAnimation();
  syncTransitionCover();
  if (m_transitionProgress <= 0.0F) {
    m_transitionProgress = 0.0F;
    m_transitionPhase = TransitionPhase::ExitEndpoint;
    syncTransitionCover();
    requestRedraw();
    return;
  }

  m_transitionPhase = TransitionPhase::Exiting;
  syncTransitionCover();
  const float durationMs = m_transitionDurationMs * m_transitionProgress;
  m_transitionAnimation = m_animations.animateTimer(
      m_transitionProgress, 0.0F, durationMs, Easing::EaseInOutCubic,
      [this](float progress) {
        m_transitionProgress = progress;
        syncTransitionCover();
      },
      [this] {
        m_transitionAnimation = 0;
        m_transitionProgress = 0.0F;
        m_transitionPhase = TransitionPhase::ExitEndpoint;
        syncTransitionCover();
        requestRedraw();
      },
      this
  );
  requestRedraw();
}

void LockSurface::suppressEntryTransition() {
  const bool entering = m_transitionPhase == TransitionPhase::Cover
      || m_transitionPhase == TransitionPhase::FinalPrime
      || m_transitionPhase == TransitionPhase::Ready
      || m_transitionPhase == TransitionPhase::Entering
      || m_transitionPhase == TransitionPhase::EnterEndpoint;
  if (entering) {
    invalidateSleepFrame();
    discardPendingFrameCallback();
  }
  cancelTransitionAnimation();
  m_enterTransitionRequested = false;
  m_transitionPhase = TransitionPhase::Disabled;
  m_transitionProgress = 1.0F;
  auto captureTexture = std::exchange(m_transitionCaptureTexture, {});
  syncTransitionCover();
  if (captureTexture.id != 0 && renderContext() != nullptr) {
    renderContext()->backend().makeCurrentNoSurface();
    renderContext()->textureManager().unload(captureTexture);
  }
  if (!m_useDesktopCaptureBackground) {
    m_desktopCapture.reset();
  }
  m_captureDirty = true;
  requestLayout();
  requestRedraw();
}

bool LockSurface::transitionInputReady() const noexcept {
  return m_transitionPhase == TransitionPhase::Disabled || m_transitionPhase == TransitionPhase::Stable;
}

bool LockSurface::exitTransitionComplete() const noexcept {
  return m_transitionPhase == TransitionPhase::Disabled || m_transitionPhase == TransitionPhase::ExitComplete;
}

void LockSurface::setBackgroundStyle(float blurIntensity) {
  if (m_blurIntensity == blurIntensity) {
    return;
  }
  m_blurIntensity = blurIntensity;
  m_captureDirty = true;
  m_blurCache.invalidate();
  m_wallpaperDirty = true;
  m_wallpaperBlurCache.invalidate();
  requestLayout();
}

void LockSurface::setBlackout(bool blackout) {
  if (m_blackout == blackout) {
    return;
  }
  invalidateSleepFrame();
  m_blackout = blackout;
  if (m_blackout) {
    m_inputDispatcher.setFocus(nullptr);
    cancelTransitionAnimation();
    m_transitionPhase = TransitionPhase::Disabled;
    m_transitionProgress = 1.0F;
    syncTransitionCover();
    notifyTransitionStateChanged();
  }
  requestLayout();
}

void LockSurface::setOnLogin(std::function<void()> onLogin) { m_onLogin = std::move(onLogin); }

void LockSurface::setOnPasswordChanged(std::function<void(const std::string&)> onPasswordChanged) {
  m_onPasswordChanged = std::move(onPasswordChanged);
}

void LockSurface::selectAllPassword() {
  if (m_passwordField == nullptr) {
    return;
  }
  m_passwordField->selectAll();
  requestLayout();
}

void LockSurface::clearPasswordSelection() {
  if (m_passwordField == nullptr) {
    return;
  }
  m_passwordField->clearSelection();
  requestLayout();
}

void LockSurface::onPointerEvent(const PointerEvent& event) {
  if (m_blackout) {
    return;
  }

  switch (event.type) {
  case PointerEvent::Type::Enter:
    m_inputDispatcher.pointerEnter(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
    break;
  case PointerEvent::Type::Leave:
    m_inputDispatcher.pointerLeave();
    break;
  case PointerEvent::Type::Motion:
    m_inputDispatcher.pointerMotion(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
    break;
  case PointerEvent::Type::Button: {
    const bool pressed = event.pressed;
    const auto x = static_cast<float>(event.sx);
    const auto y = static_cast<float>(event.sy);
    if (m_locked && pressed && passwordFieldContainsPoint(x, y)) {
      focusPasswordField();
    }
    m_inputDispatcher.pointerButton(x, y, event.button, pressed, event.serial, event.time, event.touch);
    if (m_locked && pressed && passwordFieldContainsPoint(x, y)) {
      focusPasswordField();
      requestRedraw();
    }
    break;
  }
  case PointerEvent::Type::Axis:
    m_inputDispatcher.pointerAxis(
        static_cast<float>(event.sx), static_cast<float>(event.sy), event.axis, event.axisSource, event.axisValue,
        event.axisDiscrete, event.axisValue120, event.axisLines
    );
    break;
  }

  if (m_root.paintDirty() || m_root.layoutDirty()) {
    if (m_root.layoutDirty()) {
      requestLayout();
    } else {
      requestRedraw();
    }
  }
}

void LockSurface::onThemeChanged() {
  m_captureDirty = true;
  requestLayout();
}

void LockSurface::onKeyboardEvent(const KeyboardEvent& event) {
  if (m_blackout) {
    return;
  }

  if (m_locked
      && event.pressed
      && m_passwordField != nullptr
      && m_inputDispatcher.focusedArea() != m_passwordField->inputArea()) {
    focusPasswordField();
  }
  m_inputDispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
  if (m_root.paintDirty() || m_root.layoutDirty()) {
    if (m_root.layoutDirty()) {
      requestLayout();
    } else {
      requestRedraw();
    }
  }
}

void LockSurface::handleConfigure(
    void* data, ext_session_lock_surface_v1* lockSurface, std::uint32_t serial, std::uint32_t width,
    std::uint32_t height
) {
  auto* self = static_cast<LockSurface*>(data);
  self->m_receivedConfigure = true;
  if (self->width() != width || self->height() != height) {
    self->invalidateSleepFrame();
  }
  ext_session_lock_surface_v1_ack_configure(lockSurface, serial);
  self->Surface::onConfigure(width, height);
}

bool LockSurface::ensureTransitionCaptureTexture() {
  if (m_transitionCaptureTexture.id != 0) {
    return true;
  }
  if (renderContext() == nullptr
      || !m_desktopCapture.has_value()
      || m_desktopCapture->rgba.empty()
      || m_desktopCapture->width <= 0
      || m_desktopCapture->height <= 0) {
    return false;
  }

  auto& backend = renderContext()->backend();
  if (!backend.makeCurrentNoSurface()) {
    return false;
  }
  const auto& capture = *m_desktopCapture;
  m_transitionCaptureTexture =
      renderContext()->textureManager().loadFromRgba(capture.rgba.data(), capture.width, capture.height, false);
  syncTransitionCover();
  return m_transitionCaptureTexture.id != 0;
}

void LockSurface::layoutCoverOnly(std::uint32_t width, std::uint32_t height) {
  const auto sw = static_cast<float>(width);
  const auto sh = static_cast<float>(height);
  m_root.setSize(sw, sh);
  m_backgroundLayer->setVisible(false);
  layoutTransitionCover();
  syncTransitionCover();
}

void LockSurface::layoutTransitionCover() {
  if (m_transitionCover == nullptr) {
    return;
  }
  const auto sw = static_cast<float>(width());
  const auto sh = static_cast<float>(height());
  m_transitionCover->setPosition(0.0F, 0.0F);
  m_transitionCover->setSize(sw, sh);
  m_transitionParams.aspectRatio = sh > 0.0F ? sw / sh : 1.0F;
}

void LockSurface::syncTransitionCover() {
  if (m_transitionCover == nullptr) {
    return;
  }

  const bool visible = m_transitionCaptureTexture.id != 0
      && m_transitionPhase != TransitionPhase::Disabled
      && m_transitionPhase != TransitionPhase::EnterEndpoint
      && m_transitionPhase != TransitionPhase::Stable;
  m_transitionCover->setVisible(visible);
  m_transitionCover->setTexture(m_transitionCaptureTexture.id);
  if (!visible) {
    return;
  }
  m_transitionCover->setTransition(m_transition, m_transitionProgress, m_transitionParams);
}

void LockSurface::cancelTransitionAnimation() {
  if (const auto animation = std::exchange(m_transitionAnimation, 0); animation != 0) {
    m_animations.cancel(animation);
  }
}

void LockSurface::beginEnterAnimation() {
  if (m_transitionPhase != TransitionPhase::Ready) {
    return;
  }

  cancelTransitionAnimation();
  m_transitionPhase = TransitionPhase::Entering;
  m_transitionProgress = 0.0F;
  syncTransitionCover();
  m_transitionAnimation = m_animations.animateTimer(
      0.0F, 1.0F, m_transitionDurationMs, Easing::EaseInOutCubic,
      [this](float progress) {
        m_transitionProgress = progress;
        syncTransitionCover();
      },
      [this] {
        m_transitionAnimation = 0;
        m_transitionProgress = 1.0F;
        m_transitionPhase = TransitionPhase::EnterEndpoint;
        syncTransitionCover();
        requestRedraw();
        notifyTransitionStateChanged();
      },
      this
  );
  requestRedraw();
}

void LockSurface::notifyTransitionStateChanged() {
  if (m_transitionCallback) {
    m_transitionCallback();
  }
}

void LockSurface::prepareFrame(bool needsUpdate, bool needsLayout) {
  auto* context = renderContext();
  if (context == nullptr || width() == 0 || height() == 0) {
    return;
  }

  context->makeCurrent(renderTarget());

  if (m_transitionPhase == TransitionPhase::Cover) {
    if (ensureTransitionCaptureTexture()) {
      UiPhaseScope layoutPhase(UiPhase::Layout);
      layoutCoverOnly(width(), height());
      return;
    }
    m_transitionPhase = TransitionPhase::Disabled;
    m_transitionProgress = 1.0F;
    syncTransitionCover();
  }

  if (needsUpdate) {
    UiPhaseScope updatePhase(UiPhase::Update);
    updateCopy();
  }

  if (needsUpdate || needsLayout) {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    layoutScene(width(), height());
  }
}

void LockSurface::layoutScene(std::uint32_t width, std::uint32_t height) {
  if (renderContext() == nullptr) {
    return;
  }
  Renderer& renderer = renderTarget().renderer();

  const auto sw = static_cast<float>(width);
  const auto sh = static_cast<float>(height);
  m_root.setSize(sw, sh);
  m_backgroundLayer->setVisible(true);
  m_backgroundLayer->setPosition(0.0F, 0.0F);
  m_backgroundLayer->setSize(sw, sh);
  m_backdrop->setPosition(0.0F, 0.0F);
  m_backdrop->setSize(sw, sh);

  if (m_blackout) {
    m_wallpaper->setVisible(false);
    m_backdrop->setVisible(true);
    m_backdrop->setStyle(
        RoundedRectStyle{
            .fill = rgba(0.0F, 0.0F, 0.0F, 1.0F),
            .fillMode = FillMode::Solid,
        }
    );
    m_okaru.setVisible(false);
    m_passwordField->setVisible(false);
    layoutTransitionCover();
    syncTransitionCover();
    return;
  }

  applyWallpaperTexture();

  m_wallpaper->setVisible(true);
  m_wallpaper->setPosition(0.0F, 0.0F);
  m_wallpaper->setSize(sw, sh);
  m_wallpaper->setFillMode(m_wallpaperFillMode);
  m_wallpaper->setFillColor(m_wallpaperFillColor);

  m_backdrop->setVisible(m_wallpaperFillColor.a > 0.0F);
  m_backdrop->setStyle(
      RoundedRectStyle{
          .fill = m_wallpaperFillColor,
          .fillMode = FillMode::Solid,
      }
  );

  m_passwordField->setVisible(true);
  layoutOkaru(renderer, sw, sh);
  layoutTransitionCover();
  syncTransitionCover();
}

void LockSurface::layoutOkaru(Renderer& renderer, float width, float height) {
  // Cerrar sesión, suspender, reiniciar y apagar, de las acciones de sesión configuradas y en ese orden.
  std::vector<lockscreen::OkaruLockAction> actions;
  const auto configured = resolveSessionActions();
  for (const std::string_view wanted : {"logout", "suspend", "reboot", "shutdown"}) {
    const auto it = std::ranges::find_if(configured, [wanted](const SessionPanelActionConfig& cfg) {
      return cfg.action == wanted || (wanted == "suspend" && cfg.action == "lock_and_suspend");
    });
    if (it == configured.end()) {
      continue;
    }
    actions.push_back({
        .label = it->label.has_value() && !it->label->empty() ? *it->label : i18n::tr(session_action::labelKey(wanted)),
        .icon = std::string(wanted),
        .onClick = [this, cfg = *it] {
          if (m_sessionActions != nullptr && !m_authenticating) {
            m_sessionActions->invoke(cfg);
          }
        },
    });
  }
  m_okaru.setActions(std::move(actions));

  // Estado: errores y Bloq Mayús siempre; "verificando" y mensajes de PAM mientras duran; nada en reposo.
  std::string status;
  bool statusError = false;
  if (m_error) {
    status = m_status;
    statusError = true;
  } else if (m_capsLock) {
    status = i18n::tr("lockscreen.caps-lock-on");
    statusError = true;
  } else if (m_authenticating || !m_status.empty()) {
    status = m_status;
  }

  const auto codepoints = static_cast<std::size_t>(std::ranges::count_if(m_password, [](char c) {
    return (static_cast<unsigned char>(c) & 0xC0U) != 0x80U;
  }));
  m_okaru.layout(
      renderer, width, height,
      lockscreen::OkaruLockState{
          .user = m_user,
          .passwordLength = codepoints,
          .placeholder = i18n::tr("lockscreen.okaru.password"),
          .status = std::move(status),
          .statusError = statusError,
          .authenticating = m_authenticating,
      }
  );
}

void LockSurface::updateCopy() {
  m_passwordField->setValue(m_password);
  m_passwordField->setEnabled(!m_authenticating);
}

std::vector<SessionPanelActionConfig> LockSurface::resolveSessionActions() const {
  const std::vector<SessionPanelActionConfig> src =
      m_config != nullptr ? m_config->config().shell.session.actions : defaultSessionPanelActions();
  return lockscreen::resolveSessionActions(src, defaultSessionPanelActions());
}

void LockSurface::applyWallpaperTexture() {
  if (usesDesktopCaptureBackground()) {
    applyBlurredDesktopTexture();
    if (m_blurredDesktopTexture.id != 0) {
      return;
    }
  }

  if (!m_wallpaperDirty) {
    return;
  }

  bool loaded = true;
  Color color = rgba(0.0F, 0.0F, 0.0F, 1.0F);
  if (parseColorWallpaperPath(m_wallpaperPath, color)) {
    m_wallpaperLease.reset();
    m_pendingWallpaperLease.reset();
    m_shownWallpaperPath.clear();
    m_pendingWallpaperPath.clear();
    if (m_blurredWallpaperTexture.id != 0 && renderContext() != nullptr) {
      renderContext()->backend().makeCurrentNoSurface();
      renderContext()->textureManager().unload(m_blurredWallpaperTexture);
      m_blurredWallpaperTexture = {};
    }
    m_wallpaper->setSources(
        WallpaperSourceKind::Color, {}, color, WallpaperSourceKind::Image, {}, rgba(0.0F, 0.0F, 0.0F, 1.0F), 0.0F, 0.0F,
        0.0F, 0.0F
    );
    m_wallpaper->setTransition(WallpaperTransition::Fade, 0.0F, TransitionParams{});
    m_wallpaper->setFillMode(m_wallpaperFillMode);
    m_wallpaper->setFillColor(m_wallpaperFillColor);
  } else if (m_textureCache != nullptr && !m_wallpaperPath.empty()) {
    m_wallpaperLease.setCoverage(m_wallpaperCoverage);
    m_pendingWallpaperLease.setCoverage(m_wallpaperCoverage);
    if (m_shownWallpaperPath == m_wallpaperPath) {
      m_pendingWallpaperLease.reset();
      m_pendingWallpaperPath.clear();
    } else if (m_pendingWallpaperPath != m_wallpaperPath && renderContext() != nullptr) {
      m_pendingWallpaperLease = m_textureCache->acquire(
          SharedTextureRequest{.path = m_wallpaperPath, .coverage = m_wallpaperCoverage}, &renderContext()->backend(),
          [this] {
            m_wallpaperDirty = true;
            requestLayout();
          }
      );
      m_pendingWallpaperPath = m_pendingWallpaperLease.empty() ? std::string{} : m_wallpaperPath;
    }

    if (!m_pendingWallpaperLease.empty()) {
      if (m_pendingWallpaperLease.failed()) {
        kLog.warn("failed to load lockscreen wallpaper {}", m_pendingWallpaperPath);
        m_pendingWallpaperLease.reset();
      } else if (m_pendingWallpaperLease.texture().id != 0) {
        m_wallpaperLease = std::move(m_pendingWallpaperLease);
        m_shownWallpaperPath = std::move(m_pendingWallpaperPath);
        m_pendingWallpaperPath.clear();
      }
    }

    const TextureHandle wallpaperTexture = m_wallpaperLease.texture();
    if (wallpaperTexture.id == 0) {
      loaded = false;
    } else {
      loaded = m_pendingWallpaperLease.empty();

      if (m_blurredWallpaperTexture.id != 0 && renderContext() != nullptr) {
        renderContext()->backend().makeCurrentNoSurface();
        renderContext()->textureManager().unload(m_blurredWallpaperTexture);
        m_blurredWallpaperTexture = {};
      }
      if (m_blurIntensity > 0.0F && renderContext() != nullptr) {
        auto* renderer = renderContext();
        renderer->makeCurrent(renderTarget());
        static constexpr int kBlurRounds = 3;
        const float blurRadius = m_blurIntensity * 40.0F;
        const std::uint32_t blurWidth = renderTarget().bufferWidth();
        const std::uint32_t blurHeight = renderTarget().bufferHeight();
        m_blurredWallpaperTexture = m_wallpaperBlurCache.get(
            renderer->backend(), wallpaperTexture, blurWidth, blurHeight, blurRadius, kBlurRounds
        );
      }
      const TextureHandle textureToDisplay =
          m_blurredWallpaperTexture.id != 0 ? m_blurredWallpaperTexture : wallpaperTexture;
      m_wallpaper->setTextures(
          textureToDisplay.id, {}, static_cast<float>(textureToDisplay.width),
          static_cast<float>(textureToDisplay.height), 0.0F, 0.0F
      );
      m_wallpaper->setTransition(WallpaperTransition::Fade, 0.0F, TransitionParams{});
      m_wallpaper->setFillMode(m_wallpaperFillMode);
      m_wallpaper->setFillColor(m_wallpaperFillColor);
    }
  } else if (m_wallpaperPath.empty()) {
    m_wallpaperLease.reset();
    m_pendingWallpaperLease.reset();
    m_shownWallpaperPath.clear();
    m_pendingWallpaperPath.clear();
    m_wallpaper->setTextures({}, {}, 0.0F, 0.0F, 0.0F, 0.0F);
  } else {
    loaded = false;
  }

  m_wallpaperDirty = !loaded;
}

void LockSurface::releaseCaptureTextures() {
  if (renderContext() == nullptr) {
    m_blurredWallpaperTexture = {};
    m_captureSourceTexture = {};
    m_blurredDesktopTexture = {};
    m_transitionCaptureTexture = {};
    m_blurCache.destroy();
    m_wallpaperBlurCache.destroy();
    return;
  }

  auto& tm = renderContext()->textureManager();
  renderContext()->backend().makeCurrentNoSurface();
  if (m_blurredWallpaperTexture.id != 0) {
    tm.unload(m_blurredWallpaperTexture);
    m_blurredWallpaperTexture = {};
  }
  if (m_captureSourceTexture.id != 0) {
    tm.unload(m_captureSourceTexture);
    m_captureSourceTexture = {};
  }
  if (m_blurredDesktopTexture.id != 0) {
    tm.unload(m_blurredDesktopTexture);
    m_blurredDesktopTexture = {};
  }
  if (m_transitionCaptureTexture.id != 0) {
    tm.unload(m_transitionCaptureTexture);
    m_transitionCaptureTexture = {};
  }
  m_blurCache.destroy();
  m_wallpaperBlurCache.destroy();
}

void LockSurface::applyBlurredDesktopTexture() {
  if (!m_captureDirty || !usesDesktopCaptureBackground()) {
    return;
  }

  auto* renderer = renderContext();
  if (renderer == nullptr) {
    return;
  }

  const ScreencopyImage& capture = *m_desktopCapture;
  const int texW = capture.width;
  const int texH = capture.height;
  if (texW <= 0 || texH <= 0) {
    return;
  }

  renderer->makeCurrent(renderTarget());
  auto& tm = renderer->textureManager();
  if (m_captureSourceTexture.id != 0) {
    tm.unload(m_captureSourceTexture);
    m_captureSourceTexture = {};
  }
  if (m_blurredDesktopTexture.id != 0) {
    tm.unload(m_blurredDesktopTexture);
    m_blurredDesktopTexture = {};
  }

  m_captureSourceTexture = tm.loadFromRgba(capture.rgba.data(), texW, texH, false);
  if (m_captureSourceTexture.id == 0) {
    return;
  }

  static constexpr int kBlurRounds = 3;
  const float blurRadius = m_blurIntensity * 40.0F;
  const std::uint32_t blurWidth = renderTarget().bufferWidth();
  const std::uint32_t blurHeight = renderTarget().bufferHeight();
  m_blurredDesktopTexture =
      m_blurCache.get(renderer->backend(), m_captureSourceTexture, blurWidth, blurHeight, blurRadius, kBlurRounds);
  if (m_blurredDesktopTexture.id == 0) {
    return;
  }

  m_wallpaper->setTextures(
      m_blurredDesktopTexture.id, {}, static_cast<float>(m_blurredDesktopTexture.width),
      static_cast<float>(m_blurredDesktopTexture.height), 0.0F, 0.0F
  );
  m_wallpaper->setTransition(WallpaperTransition::Fade, 0.0F, TransitionParams{});
  m_wallpaper->setFillMode(m_wallpaperFillMode);
  m_wallpaper->setFillColor(rgba(0.0F, 0.0F, 0.0F, 0.0F));
  m_backdrop->setVisible(false);
  m_captureDirty = false;
  m_wallpaperDirty = false;
}

void LockSurface::onGpuResourcesInvalidated() {
  invalidateSleepFrame();
  const bool exitWasActive =
      m_transitionPhase == TransitionPhase::Exiting || m_transitionPhase == TransitionPhase::ExitEndpoint;
  const bool entryWasActive = m_transitionPhase == TransitionPhase::Cover
      || m_transitionPhase == TransitionPhase::FinalPrime
      || m_transitionPhase == TransitionPhase::Ready
      || m_transitionPhase == TransitionPhase::Entering
      || m_transitionPhase == TransitionPhase::EnterEndpoint;
  cancelTransitionAnimation();
  releaseCaptureTextures();

  if (exitWasActive) {
    m_transitionPhase = TransitionPhase::ExitComplete;
    m_transitionProgress = 0.0F;
    notifyTransitionStateChanged();
  } else if (entryWasActive) {
    m_transitionPhase = TransitionPhase::Stable;
    m_transitionProgress = 1.0F;
  }

  m_captureDirty = true;
  m_wallpaperDirty = true;
  requestLayout();
}

void LockSurface::prepareForGraphicsReset() noexcept {
  m_blurCache.abandon();
  m_wallpaperBlurCache.abandon();
  m_blurredWallpaperTexture = {};
  m_captureSourceTexture = {};
  m_blurredDesktopTexture = {};
  m_transitionCaptureTexture = {};
  m_captureDirty = true;
  m_wallpaperDirty = true;
}

void LockSurface::forceRepaintAfterResume() {
  invalidateSleepFrame();
  discardPendingFrameCallback();
  const bool exiting =
      m_transitionPhase == TransitionPhase::Exiting || m_transitionPhase == TransitionPhase::ExitEndpoint;
  cancelTransitionAnimation();
  if (exiting) {
    m_transitionPhase = TransitionPhase::ExitComplete;
    m_transitionProgress = 0.0F;
    notifyTransitionStateChanged();
  } else if (m_transitionPhase != TransitionPhase::Disabled && m_transitionPhase != TransitionPhase::ExitComplete) {
    m_transitionPhase = TransitionPhase::Stable;
    m_transitionProgress = 1.0F;
  }
  syncTransitionCover();
  requestUpdate();
  requestRedraw();
}

void LockSurface::invalidateSleepFrame() {
  m_sleepFrameReady = false;
  if (m_sleepFrameCallback != nullptr) {
    wl_callback_destroy(m_sleepFrameCallback);
    m_sleepFrameCallback = nullptr;
  }
}

void LockSurface::render() {
  if (m_surface != nullptr
      && renderContext() != nullptr
      && renderTarget().isReady()
      && m_locked
      && !m_sleepFrameReady
      && m_sleepFrameCallback == nullptr
      && (m_transitionPhase == TransitionPhase::Disabled
          || m_transitionPhase == TransitionPhase::EnterEndpoint
          || m_transitionPhase == TransitionPhase::Stable)) {
    // This callback belongs to the uncovered buffer committed by this render, not an earlier animation frame.
    static const wl_callback_listener listener = {.done = &LockSurface::handleSleepFrameDone};
    m_sleepFrameCallback = wl_surface_frame(m_surface);
    if (m_sleepFrameCallback != nullptr) {
      wl_callback_add_listener(m_sleepFrameCallback, &listener, this);
    }
  }
  Surface::render();
}

void LockSurface::handleSleepFrameDone(void* data, wl_callback* callback, std::uint32_t /*callbackData*/) {
  auto* self = static_cast<LockSurface*>(data);
  if (callback != self->m_sleepFrameCallback) {
    return;
  }
  wl_callback_destroy(callback);
  self->m_sleepFrameCallback = nullptr;
  if (!self->m_locked
      || (self->m_transitionPhase != TransitionPhase::Disabled
          && self->m_transitionPhase != TransitionPhase::EnterEndpoint
          && self->m_transitionPhase != TransitionPhase::Stable)) {
    return;
  }
  self->m_sleepFrameReady = true;
  if (self->m_transitionPhase == TransitionPhase::EnterEndpoint) {
    self->m_transitionPhase = TransitionPhase::Stable;
    self->notifyTransitionStateChanged();
  }
  kLog.debug("lock surface ready for sleep output={}", self->m_outputKey);
  if (self->m_sleepReadyCallback) {
    self->m_sleepReadyCallback();
  }
}

void LockSurface::onFrameCallbackDone() {
  if (m_transitionPhase == TransitionPhase::Cover) {
    m_transitionPhase = TransitionPhase::FinalPrime;
    requestUpdate();
    return;
  }
  if (m_transitionPhase == TransitionPhase::FinalPrime) {
    m_transitionPhase = TransitionPhase::Ready;
    if (m_enterTransitionRequested) {
      beginEnterAnimation();
    }
    notifyTransitionStateChanged();
    return;
  }
  if (m_transitionPhase == TransitionPhase::ExitEndpoint) {
    m_transitionPhase = TransitionPhase::ExitComplete;
    notifyTransitionStateChanged();
  }
}
