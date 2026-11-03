#pragma once

// OKARU: panel flotante propio para lo que antes era una pestaña del centro de control (sonido, red,
// bluetooth, multimedia, calendario…). Hospeda la pestaña tal cual; el centro de control queda para accesos
// directos e información tipo dashboard.

#include "shell/panel/panel.h"

#include <memory>
#include <string>

class Flex;
class Tab;

class OkaruTabPanel : public Panel {
public:
  OkaruTabPanel(
      std::string titleKey, std::string glyph, std::unique_ptr<Tab> tab, float width = 460.0F, float height = 560.0F
  );
  ~OkaruTabPanel() override;

  void create() override;
  void onOpen(std::string_view context) override;
  void onClose() override;
  void onFrameTick(float deltaMs) override;
  [[nodiscard]] bool dismissTransientUi() override;
  [[nodiscard]] bool handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) override;

  [[nodiscard]] float preferredWidth() const override { return scaled(m_width); }
  void onPanelCardOpacityChanged(float opacity) override; // OKARU: misma opacidad de tarjeta que el resto
  [[nodiscard]] float preferredHeight() const override { return scaled(m_height); }

private:
  void doLayout(Renderer& renderer, float width, float height) override;
  void doUpdate(Renderer& renderer) override;

  std::string m_titleKey;
  std::string m_glyph;
  std::unique_ptr<Tab> m_tab;
  float m_width;
  float m_height;
  Flex* m_rootLayout = nullptr;
  Flex* m_body = nullptr;
  Flex* m_container = nullptr;
};
