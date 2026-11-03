#pragma once

// OKARU: gato animado de la barra (antes el plugin dotnetrob/cat, MIT). Corre más rápido cuanto más CPU se
// usa: dormido bajo walk_threshold, caminando hasta run_threshold y corriendo por encima. La figura es una
// fuente (assets/fonts/okaru-cat.otf, trazada del plasmoid CatWalk de Driglu4it, MIT): "a" = dormido,
// "b".."f" = ciclo de carrera. Clic: panel de Sistema.

#include "shell/bar/widget.h"

#include <cstdint>
#include <memory>

class Label;
class SystemMonitorService;
class Timer;
struct wl_output;

class OkaruCatWidget : public Widget {
public:
  struct Options {
    std::int64_t catSize = 24;
    bool showCpuPercent = false;
    std::int64_t walkThreshold = 15;
    std::int64_t runThreshold = 60;
  };

  OkaruCatWidget(wl_output* output, Options options, SystemMonitorService* sysmon);
  ~OkaruCatWidget() override;

  void create() override;

  // Paso del gato según la CPU: 0 = dormido, 1 = camina, 2 = corre; frameMs = duración de cada fotograma.
  struct Pace {
    int mode = 0;
    float frameMs = 380.0F;
  };
  [[nodiscard]] static Pace paceFor(double cpu, std::int64_t walkThreshold, std::int64_t runThreshold);

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void tick();

  Options m_options;
  SystemMonitorService* m_sysmon = nullptr;
  Label* m_cat = nullptr;
  Label* m_percent = nullptr;
  std::unique_ptr<Timer> m_timer;
  double m_cpu = 0.0;
  int m_frame = 0;
  float m_frameElapsed = 0.0F;
  float m_sampleElapsed = 1e9F;
};
