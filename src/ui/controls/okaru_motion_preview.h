#pragma once

// OKARU: vista previa de una curva de animación (como la de dux-shell): al tocarla, una pastilla
// cruza el carril siguiendo la curva, ida y vuelta.

#include "render/scene/node.h"
#include "ui/signal.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class InputArea;
class Label;
class RectNode;

class OkaruMotionPreview : public Node {
public:
  // Estado de la reproducción fuera del widget: settings se reconstruye entero en cada cambio (a
  // veces dos veces seguidas), y la vista previa nueva debe seguir donde iba la anterior.
  struct Clock {
    std::chrono::steady_clock::time_point startedAt{};
    bool towardEnd = false; // hacia dónde va (o fue) la última reproducción
    bool running = false;
  };

  // `progress(ms)` devuelve 0 al inicio y 1 en el destino (puede pasarse si la curva rebota).
  OkaruMotionPreview(
      std::function<double(double)> progress, double durationMs, std::string hint, float scale,
      std::shared_ptr<Clock> clock
  );
  ~OkaruMotionPreview() override;

  // Reproduce hacia el otro lado.
  void play();

protected:
  LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;
  void doLayout(Renderer& renderer) override;

private:
  void run(double elapsedMs);
  void applyPalette();
  void placePill();

  std::function<double(double)> m_progress;
  double m_durationMs = 300.0;
  float m_scale = 1.0F;
  float m_height = 56.0F;
  double m_value = 0.0; // 0 = izquierda, 1 = derecha
  std::shared_ptr<Clock> m_clock;
  std::uint32_t m_animId = 0; // AnimationManager::Id
  bool m_resumePending = false;

  RectNode* m_lane = nullptr;
  RectNode* m_pill = nullptr;
  Label* m_hint = nullptr;
  InputArea* m_inputArea = nullptr;
  Signal<>::ScopedConnection m_paletteConn;
};
