#pragma once

// OKARU: panel de red (antes mahmoudomiesh/network-monitor:panel): velocidad actual y puertos en escucha, con
// abrir en el navegador y detener el proceso. La gráfica de tráfico está en el panel Sistema.

#include "core/timer_manager.h"
#include "shell/control_center/tab.h"

#include <string>

class Flex;
class Label;
class SystemMonitorService;

class OkaruPortsTab : public Tab {
public:
  explicit OkaruPortsTab(SystemMonitorService* sysmon);
  ~OkaruPortsTab() override;

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void setActive(bool active) override;
  void onClose() override;

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void rebuildRows();

  SystemMonitorService* m_sysmon = nullptr;
  Flex* m_root = nullptr;
  Flex* m_rows = nullptr;
  Label* m_rx = nullptr;
  Label* m_tx = nullptr;
  Label* m_count = nullptr;
  std::string m_rowsKey;
  int m_listener = 0;
  Timer m_timer;
};
