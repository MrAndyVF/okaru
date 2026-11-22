#pragma once

// OKARU: detecta que el usuario agita el ratón para encontrar el cursor (como «shake cursor» de KWin). Con las
// posiciones del último segundo: agitar es recorrer mucho más camino que el tamaño de la zona que se cubre, con
// varios cambios de sentido. Lógica pura (sin Wayland) para poder probarla.

#include <algorithm>
#include <cmath>
#include <deque>

namespace okaru::cursor_shake {

  class ShakeDetector {
  public:
    static constexpr double kWindowMs = 1000.0;
    static constexpr double kMinPath = 1400.0; // px lógicos recorridos en la ventana
    static constexpr double kMinRatio = 4.0;   // recorrido / diagonal de la zona cubierta
    static constexpr int kMinReversals = 4;    // cambios de sentido en el eje dominante
    static constexpr double kMinStep = 4.0;    // px: los movimientos menores no cuentan para el sentido

    // Añade una muestra (ms, px lógicos globales); devuelve true si con ella el movimiento es una agitación.
    bool update(double timeMs, double x, double y) {
      m_samples.push_back({timeMs, x, y});
      while (!m_samples.empty() && timeMs - m_samples.front().t > kWindowMs) {
        m_samples.pop_front();
      }
      if (m_samples.size() < 4) {
        return false;
      }
      double path = 0.0;
      double minX = m_samples.front().x;
      double maxX = minX;
      double minY = m_samples.front().y;
      double maxY = minY;
      for (std::size_t i = 1; i < m_samples.size(); ++i) {
        const Sample& a = m_samples[i - 1];
        const Sample& b = m_samples[i];
        path += std::hypot(b.x - a.x, b.y - a.y);
        minX = std::min(minX, b.x);
        maxX = std::max(maxX, b.x);
        minY = std::min(minY, b.y);
        maxY = std::max(maxY, b.y);
      }
      const double diagonal = std::hypot(maxX - minX, maxY - minY);
      if (path < kMinPath || diagonal <= 0.0 || path / diagonal < kMinRatio) {
        return false;
      }
      return reversals(maxX - minX >= maxY - minY) >= kMinReversals;
    }

    void reset() { m_samples.clear(); }

  private:
    struct Sample {
      double t;
      double x;
      double y;
    };

    [[nodiscard]] int reversals(bool horizontal) const {
      int count = 0;
      int lastSign = 0;
      for (std::size_t i = 1; i < m_samples.size(); ++i) {
        const double delta = horizontal ? m_samples[i].x - m_samples[i - 1].x : m_samples[i].y - m_samples[i - 1].y;
        if (std::abs(delta) < kMinStep) {
          continue;
        }
        const int sign = delta > 0.0 ? 1 : -1;
        if (lastSign != 0 && sign != lastSign) {
          ++count;
        }
        lastSign = sign;
      }
      return count;
    }

    std::deque<Sample> m_samples;
  };

} // namespace okaru::cursor_shake
