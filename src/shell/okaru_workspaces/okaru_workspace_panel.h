#pragma once

// OKARU: menú flotante del clic derecho en un workspace de la barra: renombrar y cambiar el icono.
// El contexto del panel es el id del workspace. El selector de glifos va dentro del propio menú.

#include "compositors/hyprland/hyprland_runtime.h"
#include "shell/panel/panel.h"

#include <string>

class ConfigService;
class Flex;

class OkaruWorkspacePanel : public Panel {
public:
  explicit OkaruWorkspacePanel(ConfigService* config);

  void create() override;
  void onOpen(std::string_view context) override;
  void onClose() override;
  [[nodiscard]] bool isContextActive(std::string_view context) const override { return context == m_context; }

  [[nodiscard]] float preferredWidth() const override;
  [[nodiscard]] float preferredHeight() const override;
  [[nodiscard]] bool fitsContentHeight() const noexcept override { return true; }
  [[nodiscard]] float measureContentHeight(Renderer& renderer, float contentWidth) override;
  [[nodiscard]] LayerShellKeyboard keyboardMode() const override { return LayerShellKeyboard::Exclusive; }

private:
  void doLayout(Renderer& renderer, float width, float height) override;
  void doUpdate(Renderer& renderer) override;
  void rebuild();
  void markDirty();
  [[nodiscard]] std::string currentIcon() const;

  ConfigService* m_config = nullptr;
  compositors::hyprland::HyprlandRuntime m_runtime;
  std::string m_context;
  int m_id = 0;
  bool m_picking = false;
  bool m_dirty = true;
  std::string m_error;
  Flex* m_rootLayout = nullptr;
};
