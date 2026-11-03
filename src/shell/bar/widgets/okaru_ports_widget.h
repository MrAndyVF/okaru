#pragma once

// OKARU: indicador de red de la barra (antes mahmoudomiesh/network-monitor:indicator): velocidad de bajada y
// subida y número de puertos en escucha. Clic: panel okaru-ports. Clic central: volver a escanear.

#include "shell/bar/widget.h"

#include <cstdint>
#include <memory>

class Flex;
class Glyph;
class Label;
class SystemMonitorService;
class Timer;
struct wl_output;

class OkaruPortsWidget : public Widget {
public:
  struct Options {
    bool showSpeed = true;
    bool showPorts = true;
    std::int64_t refreshInterval = 5;
    bool hideSystemPorts = true;
    bool onlyOwnProcesses = false;
    bool includeUdp = false;
  };

  OkaruPortsWidget(wl_output* output, Options options, SystemMonitorService* sysmon);
  ~OkaruPortsWidget() override;

  void create() override;

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void sync();

  Options m_options;
  SystemMonitorService* m_sysmon = nullptr;
  Glyph* m_rxIcon = nullptr;
  Label* m_rx = nullptr;
  Glyph* m_txIcon = nullptr;
  Label* m_tx = nullptr;
  Label* m_count = nullptr;
  Flex* m_portsGroup = nullptr;
  int m_listener = 0;
  std::unique_ptr<Timer> m_timer;
};
