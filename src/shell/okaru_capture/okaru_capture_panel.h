#pragma once

// OKARU: menú flotante del widget Captura. Pestañas Captura (pantalla, región, ventana) y Grabación
// (grabar/detener, replay y opciones rápidas que se guardan en [widget.capture]).

#include "core/timer_manager.h"
#include "shell/panel/panel.h"

#include <functional>
#include <string>

class ConfigService;
class Flex;
class WaylandConnection;

class OkaruCapturePanel : public Panel {
public:
  OkaruCapturePanel(
      ConfigService* config, WaylandConnection* wayland, std::function<void(const std::string&)> runCommand
  );
  ~OkaruCapturePanel() override;

  void create() override;
  void onOpen(std::string_view context) override;
  void onClose() override;

  [[nodiscard]] float preferredWidth() const override { return scaled(440.0F); }
  [[nodiscard]] float preferredHeight() const override { return scaled(520.0F); }
  [[nodiscard]] bool fitsContentHeight() const noexcept override { return true; }
  [[nodiscard]] float measureContentHeight(Renderer& renderer, float contentWidth) override;

private:
  void doLayout(Renderer& renderer, float width, float height) override;
  void doUpdate(Renderer& renderer) override;
  void rebuild();
  void markDirty();
  void addCaptureTab(Flex& body, float scale);
  void addRecordTab(Flex& body, float scale);
  void runAfterClose(std::string command);
  void setOption(const std::string& key, const std::string& value);
  void setOption(const std::string& key, std::int64_t value);

  ConfigService* m_config = nullptr;
  WaylandConnection* m_wayland = nullptr;
  std::function<void(const std::string&)> m_runCommand;
  Flex* m_rootLayout = nullptr;
  Flex* m_body = nullptr;
  std::size_t m_tab = 0; // 0 = Captura, 1 = Grabación
  bool m_dirty = true;
  int m_listener = 0;
  Timer m_tick;
  Timer m_deferred;
};
