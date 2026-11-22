#include "shell/okaru_cursor_shake/okaru_cursor_shake.h"

#include "compositors/compositor_detect.h"
#include "config/config_service.h"
#include "config/config_types.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "render/core/texture_manager.h"
#include "render/render_context.h"
#include "render/scene/node.h"
#include "ui/builders.h"
#include "ui/controls/image.h"
#include "wayland/layer_surface.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <cstdlib>

namespace {

  constexpr Logger kLog("cursor-shake");
  constexpr auto kPollInterval = std::chrono::milliseconds(33);
  constexpr auto kFrameInterval = std::chrono::milliseconds(16);
  constexpr auto kSettleDelay = std::chrono::milliseconds(600); // sin agitar este tiempo: encoge
  constexpr float kAnimMs = 180.0F;
  constexpr float kMaxFactor = 4.0F;

  double nowMs() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
  }

  float easeOut(float t) { return 1.0F - (1.0F - t) * (1.0F - t) * (1.0F - t); }

} // namespace

OkaruCursorShake::~OkaruCursorShake() {
  m_config = nullptr; // al cerrar la shell no se vuelve a sondear
  hide();
}

void OkaruCursorShake::initialize(WaylandConnection& wayland, ConfigService* config, RenderContext* renderContext) {
  m_wayland = &wayland;
  m_config = config;
  m_renderContext = renderContext;
  if (const char* size = std::getenv("XCURSOR_SIZE"); size != nullptr) {
    m_baseSize = std::clamp(static_cast<float>(std::strtol(size, nullptr, 10)), 8.0F, 256.0F);
  }
  onConfigReload();
}

bool OkaruCursorShake::enabled() const {
  return m_config != nullptr && m_config->config().accessibility.cursorShake && compositors::isHyprland();
}

void OkaruCursorShake::onConfigReload() {
  if (!enabled()) {
    m_pollTimer.stop();
    hide();
    return;
  }
  if (!m_pollTimer.active() && m_phase == Phase::Hidden) {
    m_detector.reset();
    m_pollTimer.startRepeating(kPollInterval, [this] { poll(); });
  }
}

void OkaruCursorShake::onOutputChange() { hide(); }

void OkaruCursorShake::poll() {
  const auto json = m_hyprland.requestJson("j/cursorpos");
  if (!json.has_value() || !json->is_object() || !json->contains("x") || !json->contains("y")) {
    return;
  }
  const double x = json->value("x", 0.0);
  const double y = json->value("y", 0.0);
  if (m_detector.update(nowMs(), x, y)) {
    show(x, y);
  }
}

void OkaruCursorShake::show(double globalX, double globalY) {
  if (!m_cursorLoaded) {
    m_cursorLoaded = true;
    const char* theme = std::getenv("XCURSOR_THEME");
    m_cursor = okaru::cursor_shake::loadThemeArrow(theme != nullptr && theme[0] != '\0' ? theme : "default");
    if (!m_cursor.has_value()) {
      kLog.warn("no Xcursor arrow found for theme {}; cursor shake disabled", theme != nullptr ? theme : "default");
    }
  }
  if (!m_cursor.has_value() || m_wayland == nullptr) {
    return;
  }
  m_pollTimer.stop(); // mientras las capas están, el movimiento llega por wl_pointer
  for (const auto& out : m_wayland->outputs()) {
    if (!out.done || out.output == nullptr) {
      continue;
    }
    auto overlay = std::make_unique<Overlay>();
    overlay->originX = static_cast<float>(out.logicalX);
    overlay->originY = static_cast<float>(out.logicalY);
    overlay->surface = std::make_unique<LayerSurface>(
        *m_wayland, LayerSurfaceConfig{
                        .nameSpace = "okaru-cursor-shake",
                        .layer = LayerShellLayer::Overlay,
                        .anchor = LayerShellAnchor::Top | LayerShellAnchor::Bottom | LayerShellAnchor::Left
                            | LayerShellAnchor::Right,
                        .exclusiveZone = -1,
                        .keyboard = LayerShellKeyboard::None,
                    }
    );
    Overlay* raw = overlay.get();
    raw->surface->setRenderContext(m_renderContext);
    raw->surface->setConfigureCallback([raw](std::uint32_t, std::uint32_t) { raw->surface->requestLayout(); });
    raw->surface->setPrepareFrameCallback([this, raw](bool, bool) { prepareFrame(*raw); });
    if (!raw->surface->initialize(out.output)) {
      kLog.warn("cursor shake: failed to create the overlay on {}", out.connectorName);
      continue;
    }
    if (globalX >= out.logicalX && globalX < out.logicalX + out.logicalWidth && globalY >= out.logicalY
        && globalY < out.logicalY + out.logicalHeight) {
      m_active = raw;
      m_pointerX = static_cast<float>(globalX) - raw->originX;
      m_pointerY = static_cast<float>(globalY) - raw->originY;
    }
    m_overlays.push_back(std::move(overlay));
  }
  if (m_active == nullptr) {
    hide();
    return;
  }
  startAnimation(Phase::Growing);
  m_settleTimer.start(kSettleDelay, [this] { startAnimation(Phase::Shrinking); });
}

void OkaruCursorShake::hide() {
  m_animTimer.stop();
  m_settleTimer.stop();
  m_active = nullptr;
  for (auto& overlay : m_overlays) {
    overlay->surface->setSceneRoot(nullptr);
  }
  m_overlays.clear();
  m_phase = Phase::Hidden;
  m_factor = 1.0F;
  m_detector.reset();
  if (enabled() && !m_pollTimer.active()) {
    m_pollTimer.startRepeating(kPollInterval, [this] { poll(); });
  }
}

void OkaruCursorShake::prepareFrame(Overlay& overlay) {
  auto& surface = *overlay.surface;
  if (m_renderContext == nullptr || surface.width() == 0 || surface.height() == 0) {
    return;
  }
  m_renderContext->makeCurrent(surface.renderTarget());
  Renderer& renderer = surface.renderTarget().renderer();
  UiPhaseScope layoutPhase(UiPhase::Layout);
  if (overlay.root == nullptr) {
    overlay.root = ui::node({.width = static_cast<float>(surface.width()), .height = static_cast<float>(surface.height())});
    overlay.image = static_cast<Image*>(overlay.root->addChild(ui::image({.fit = ImageFit::Stretch})));
    surface.setSceneRoot(overlay.root.get());
  }
  if (!overlay.imageReady) {
    const auto& cursor = *m_cursor;
    overlay.imageReady = overlay.image->setSourceRaw(
        renderer, cursor.rgba.data(), cursor.rgba.size(), cursor.width, cursor.height, cursor.width * 4,
        PixmapFormat::RGBA, true
    );
  }
  overlay.image->setVisible(&overlay == m_active);
  if (&overlay == m_active) {
    place(m_pointerX, m_pointerY);
  }
  overlay.root->layout(renderer);
}

void OkaruCursorShake::relayout() {
  for (auto& overlay : m_overlays) {
    overlay->surface->requestLayout();
  }
}

void OkaruCursorShake::place(float x, float y) {
  m_pointerX = x;
  m_pointerY = y;
  if (m_active == nullptr || m_active->image == nullptr || !m_cursor.has_value()) {
    return;
  }
  const auto& cursor = *m_cursor;
  // La imagen del tema corresponde a nominalSize: se escala al tamaño del cursor del usuario por el factor.
  const float s = m_baseSize * m_factor / static_cast<float>(std::max(1, cursor.nominalSize));
  m_active->image->setSize(static_cast<float>(cursor.width) * s, static_cast<float>(cursor.height) * s);
  m_active->image->setPosition(x - static_cast<float>(cursor.hotX) * s, y - static_cast<float>(cursor.hotY) * s);
}

void OkaruCursorShake::startAnimation(Phase phase) {
  if (m_phase == phase || (phase == Phase::Growing && m_phase == Phase::Shown)) {
    return;
  }
  m_phase = phase;
  m_animFrom = m_factor;
  m_animStart = std::chrono::steady_clock::now();
  m_animTimer.startRepeating(kFrameInterval, [this] { tick(); });
}

void OkaruCursorShake::tick() {
  const float elapsed =
      std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - m_animStart).count();
  const float t = std::clamp(elapsed / kAnimMs, 0.0F, 1.0F);
  const float target = m_phase == Phase::Shrinking ? 1.0F : kMaxFactor;
  m_factor = m_animFrom + (target - m_animFrom) * easeOut(t);
  if (t >= 1.0F) {
    m_animTimer.stop();
    if (m_phase == Phase::Shrinking) {
      hide();
      return;
    }
    m_phase = Phase::Shown;
  }
  if (m_active != nullptr) {
    place(m_pointerX, m_pointerY);
    m_active->surface->requestLayout();
  }
}

void OkaruCursorShake::feedDetector(float localX, float localY) {
  // wl_pointer llega a la frecuencia del ratón (cientos por segundo): pasos de pocos px que el detector no cuenta
  // como cambio de sentido. Se muestrea al mismo ritmo que el sondeo por IPC, con el que se ajustaron los umbrales.
  const double now = nowMs();
  if (now - m_lastSampleMs < static_cast<double>(kPollInterval.count())) {
    return;
  }
  m_lastSampleMs = now;
  if (m_active != nullptr && m_detector.update(now, localX + m_active->originX, localY + m_active->originY)) {
    // Sigue agitando: vuelve a crecer si estaba encogiendo y se aplaza el encogido.
    if (m_phase == Phase::Shrinking) {
      startAnimation(Phase::Growing);
    }
    m_settleTimer.start(kSettleDelay, [this] { startAnimation(Phase::Shrinking); });
  }
}

bool OkaruCursorShake::onPointerEvent(const PointerEvent& event) {
  if (event.surface == nullptr) {
    return false;
  }
  const auto it = std::ranges::find_if(m_overlays, [&](const auto& overlay) {
    return overlay->surface->wlSurface() == event.surface;
  });
  if (it == m_overlays.end()) {
    return false;
  }
  Overlay* overlay = it->get();
  const auto x = static_cast<float>(event.sx);
  const auto y = static_cast<float>(event.sy);
  switch (event.type) {
  case PointerEvent::Type::Enter:
    // Cruzó a esta pantalla (o la capa apareció bajo el puntero): el cursor grande se dibuja aquí.
    m_wayland->hideCursor(); // OKARU: sin el cursor del sistema debajo del agrandado
    m_active = overlay;
    place(x, y);
    relayout();
    break;
  case PointerEvent::Type::Motion:
    if (overlay == m_active) {
      place(x, y);
      feedDetector(x, y);
      overlay->surface->requestLayout();
    }
    break;
  case PointerEvent::Type::Button:
    if (event.pressed && m_phase != Phase::Shrinking) {
      // Se encontró el cursor: las capas dejan pasar el puntero (vuelve el cursor del sistema) y el grande encoge.
      for (auto& each : m_overlays) {
        each->surface->setClickThrough(true);
      }
      m_settleTimer.stop();
      startAnimation(Phase::Shrinking);
    }
    break;
  case PointerEvent::Type::Leave:
  case PointerEvent::Type::Axis:
    break;
  }
  return true;
}
