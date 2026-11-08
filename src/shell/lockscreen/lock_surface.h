#pragma once

#include "capture/screencopy_capture.h"
#include "config/config_service.h"
#include "render/animation/animation_manager.h"
#include "render/core/blur_cache.h"
#include "render/core/color.h"
#include "render/core/lockscreen_transition_types.h"
#include "render/core/shared_texture_cache.h"
#include "render/core/texture_manager.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/node.h"
#include "shell/lockscreen/okaru_lock_view.h"
#include "wayland/surface.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct ext_session_lock_surface_v1;
struct ext_session_lock_v1;
struct wl_output;

class Box;
class Input;
class LockscreenTransitionCover;
class Renderer;
class SessionActionRunner;
class WallpaperNode;
struct KeyboardEvent;
struct PointerEvent;
struct SessionPanelActionConfig;

class LockSurface : public Surface {
public:
  explicit LockSurface(WaylandConnection& connection, ConfigService* config = nullptr);
  ~LockSurface() override;

  using Surface::initialize;
  bool initialize() override { return false; }
  bool initialize(ext_session_lock_v1* lock, wl_output* output, std::int32_t scale);
  void setLockedState(bool locked);
  void setPromptState(std::string user, std::string password, std::string status, bool error, bool authenticating);
  void setCapsLock(bool capsLock);
  void setTextureCache(SharedTextureCache* cache) noexcept { m_textureCache = cache; }
  void setWallpaperPath(std::string wallpaperPath);
  void setWallpaperFillMode(WallpaperFillMode fillMode);
  void setWallpaperCoverage(TextureCoverage coverage);
  void setWallpaperFillColor(Color fillColor);
  void setDesktopCapture(std::optional<ScreencopyImage> capture, bool useAsBackground);
  void configureTransition(
      std::optional<LockscreenTransitionKind> transition, const LockscreenTransitionParams& params, float durationMs
  );
  void startEnterTransition();
  void suppressEntryTransition();
  void startExitTransition();
  [[nodiscard]] bool transitionInputReady() const noexcept;
  [[nodiscard]] bool exitTransitionComplete() const noexcept;
  void setTransitionCallback(std::function<void()> callback) { m_transitionCallback = std::move(callback); }
  void setBackgroundStyle(float blurIntensity);
  void setBlackout(bool blackout);
  [[nodiscard]] bool isBlackout() const noexcept { return m_blackout; }
  void setOnLogin(std::function<void()> onLogin);
  void setOnPasswordChanged(std::function<void(const std::string&)> onPasswordChanged);
  void setSessionActions(SessionActionRunner* sessionActions);
  void selectAllPassword();
  void clearPasswordSelection();
  void onThemeChanged();
  void onGpuResourcesInvalidated();
  void prepareForGraphicsReset() noexcept;
  void forceRepaintAfterResume();
  void onPointerEvent(const PointerEvent& event);
  void onKeyboardEvent(const KeyboardEvent& event);
  [[nodiscard]] wl_output* output() const noexcept { return m_output; }
  void syncOutputScale(std::int32_t bufferScale, std::uint32_t configuredScaleNumerator);
  [[nodiscard]] bool usesDesktopCaptureBackground() const noexcept;
  void setOutputKey(std::string outputKey) { m_outputKey = std::move(outputKey); }

  [[nodiscard]] bool readyForSleep() const noexcept { return m_sleepFrameReady; }
  void setSleepReadyCallback(std::function<void()> callback) { m_sleepReadyCallback = std::move(callback); }

  static void handleConfigure(
      void* data, ext_session_lock_surface_v1* lockSurface, std::uint32_t serial, std::uint32_t width,
      std::uint32_t height
  );

protected:
  void render() override;
  void onFrameCallbackDone() override;

private:
  enum class TransitionPhase : std::uint8_t {
    Disabled,
    Cover,
    FinalPrime,
    Ready,
    Entering,
    EnterEndpoint,
    Stable,
    Exiting,
    ExitEndpoint,
    ExitComplete,
  };

  void prepareFrame(bool needsUpdate, bool needsLayout);
  [[nodiscard]] bool ensureTransitionCaptureTexture();
  void layoutCoverOnly(std::uint32_t width, std::uint32_t height);
  void layoutTransitionCover();
  void syncTransitionCover();
  void cancelTransitionAnimation();
  void beginEnterAnimation();
  void notifyTransitionStateChanged();
  void invalidateSleepFrame();
  static void handleSleepFrameDone(void* data, wl_callback* callback, std::uint32_t callbackData);
  void applyWallpaperTexture();
  void applyBlurredDesktopTexture();
  void releaseCaptureTextures();
  void layoutScene(std::uint32_t width, std::uint32_t height);
  void updateCopy();
  [[nodiscard]] std::vector<SessionPanelActionConfig> resolveSessionActions() const;
  [[nodiscard]] bool passwordFieldContainsPoint(float sceneX, float sceneY) const;
  void focusPasswordField();

  ext_session_lock_surface_v1* m_lockSurface = nullptr;
  wl_output* m_output = nullptr;
  bool m_receivedConfigure = false;
  ConfigService* m_config = nullptr;
  // Declared before m_root so it outlives the scene: ~Node cancels its animations through this manager.
  AnimationManager m_animations;
  Node m_root;
  Node* m_backgroundLayer = nullptr;
  LockscreenTransitionCover* m_transitionCover = nullptr;
  WallpaperNode* m_wallpaper = nullptr;
  Box* m_backdrop = nullptr;
  Input* m_passwordField = nullptr;
  // OKARU: diseño propio igual al greeter (okaru_lock_view.h).
  void layoutOkaru(Renderer& renderer, float width, float height);
  lockscreen::OkaruLockView m_okaru;
  SharedTextureCache* m_textureCache = nullptr;
  SharedTextureCache::Lease m_wallpaperLease;
  SharedTextureCache::Lease m_pendingWallpaperLease;
  TextureHandle m_blurredWallpaperTexture{};
  TextureHandle m_captureSourceTexture{};
  TextureHandle m_blurredDesktopTexture{};
  TextureHandle m_transitionCaptureTexture{};
  BlurCache m_blurCache;
  BlurCache m_wallpaperBlurCache;
  std::optional<ScreencopyImage> m_desktopCapture;
  LockscreenTransitionKind m_transition = LockscreenTransitionKind::Fade;
  LockscreenTransitionParams m_transitionParams;
  TransitionPhase m_transitionPhase = TransitionPhase::Disabled;
  AnimationManager::Id m_transitionAnimation = 0;
  float m_transitionProgress = 1.0F;
  float m_transitionDurationMs = 1500.0F;
  float m_blurIntensity = 0.5F;
  bool m_blackout = false;
  bool m_useDesktopCaptureBackground = false;
  bool m_enterTransitionRequested = false;
  bool m_captureDirty = true;
  std::string m_wallpaperPath;
  std::string m_shownWallpaperPath;
  std::string m_pendingWallpaperPath;
  WallpaperFillMode m_wallpaperFillMode = WallpaperFillMode::Crop;
  TextureCoverage m_wallpaperCoverage;
  Color m_wallpaperFillColor = rgba(0.0F, 0.0F, 0.0F, 0.0F);
  bool m_wallpaperDirty = false;
  InputDispatcher m_inputDispatcher;
  std::function<void()> m_onLogin;
  std::function<void(const std::string&)> m_onPasswordChanged;
  bool m_locked = false;
  std::string m_user;
  std::string m_password;
  std::string m_status;
  bool m_error = false;
  bool m_authenticating = false;
  bool m_capsLock = false;
  std::string m_outputKey;
  wl_callback* m_sleepFrameCallback = nullptr;
  bool m_sleepFrameReady = false;
  std::function<void()> m_sleepReadyCallback;
  std::function<void()> m_transitionCallback;

  SessionActionRunner* m_sessionActions = nullptr;
};
