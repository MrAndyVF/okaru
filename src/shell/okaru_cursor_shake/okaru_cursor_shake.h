#pragma once

// OKARU: agrandar el cursor al agitar el ratón (accessibility.cursor_shake), dibujado por la shell. La posición se pide
// a Hyprland por IPC (~30 Hz) solo para detectar la agitación; al detectarla, una capa overlay transparente en cada
// pantalla toma el puntero (el cursor grande se dibuja en la que lo tiene y cruza entre pantallas), oculta el cursor
// del sistema y dibuja la flecha del tema del usuario creciendo y siguiendo el movimiento sin retraso. Al dejar de
// agitar encoge y las capas desaparecen. Un clic (que se pierde: la función es para encontrar el cursor) deja pasar el
// puntero y encoge en el sitio. En Niri no hay posición del cursor por IPC: queda desactivada.

#include "compositors/hyprland/hyprland_runtime.h"
#include "core/timer_manager.h"
#include "shell/okaru_cursor_shake/okaru_shake_detector.h"
#include "shell/okaru_cursor_shake/okaru_xcursor.h"

#include <chrono>
#include <memory>
#include <optional>
#include <vector>

class ConfigService;
class Image;
class LayerSurface;
class Node;
class RenderContext;
class PopupGrabHost;
class WaylandConnection;
struct PointerEvent;
struct wl_output;

class OkaruCursorShake {
public:
  OkaruCursorShake() = default;
  ~OkaruCursorShake();

  OkaruCursorShake(const OkaruCursorShake&) = delete;
  OkaruCursorShake& operator=(const OkaruCursorShake&) = delete;

  void initialize(WaylandConnection& wayland, ConfigService* config, RenderContext* renderContext);
  void onConfigReload();
  void onOutputChange();
  bool onPointerEvent(const PointerEvent& event);

private:
  enum class Phase : std::uint8_t { Hidden, Growing, Shown, Shrinking };

  // Una capa por pantalla; la imagen solo se ve en la que tiene el puntero.
  struct Overlay {
    std::unique_ptr<LayerSurface> surface;
    std::unique_ptr<Node> root;
    Image* image = nullptr;
    bool imageReady = false;
    float originX = 0.0F; // origen lógico de la pantalla
    float originY = 0.0F;
  };

  [[nodiscard]] bool enabled() const;
  void poll();
  void show(double globalX, double globalY);
  void hide();
  [[nodiscard]] PopupGrabHost* grabHost() const;
  void prepareFrame(Overlay& overlay);
  void relayout();
  void startAnimation(Phase phase);
  void tick();
  void place(float x, float y);
  void feedDetector(float localX, float localY);

  WaylandConnection* m_wayland = nullptr;
  ConfigService* m_config = nullptr;
  RenderContext* m_renderContext = nullptr;
  compositors::hyprland::HyprlandRuntime m_hyprland;
  okaru::cursor_shake::ShakeDetector m_detector;
  std::optional<okaru::cursor_shake::CursorImage> m_cursor;
  bool m_cursorLoaded = false;
  float m_baseSize = 24.0F; // XCURSOR_SIZE

  std::vector<std::unique_ptr<Overlay>> m_overlays;
  Overlay* m_active = nullptr; // la que tiene el puntero
  float m_pointerX = 0.0F;     // posición en la capa activa
  float m_pointerY = 0.0F;

  Phase m_phase = Phase::Hidden;
  float m_factor = 1.0F;
  float m_animFrom = 1.0F;
  double m_lastSampleMs = 0.0; // última muestra que recibió el detector desde wl_pointer
  std::chrono::steady_clock::time_point m_animStart;
  Timer m_pollTimer;
  Timer m_animTimer;
  Timer m_settleTimer;
};
