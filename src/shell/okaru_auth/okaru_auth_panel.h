#pragma once

// OKARU: diálogo de contraseña de la shell. Muestra la solicitud actual de AuthPromptService.

#include "shell/panel/panel.h"

#include <string>

class Button;
class ConfigService;
class Flex;
class Input;
class InputArea;
class Label;

namespace okaru::auth {
  class AuthPromptService;
}

class OkaruAuthPanel : public Panel {
public:
  OkaruAuthPanel(ConfigService* config, okaru::auth::AuthPromptService* service);

  void create() override;
  void onOpen(std::string_view context) override;
  void onClose() override;

  [[nodiscard]] float preferredWidth() const override { return scaled(480.0F); }
  [[nodiscard]] float preferredHeight() const override { return scaled(560.0F); }
  [[nodiscard]] bool fitsContentHeight() const noexcept override { return true; }
  [[nodiscard]] float measureContentHeight(Renderer& renderer, float contentWidth) override;
  [[nodiscard]] PanelPlacement panelPlacement() const noexcept override;
  [[nodiscard]] LayerShellLayer layer() const override { return LayerShellLayer::Overlay; }
  [[nodiscard]] LayerShellKeyboard keyboardMode() const override { return LayerShellKeyboard::Exclusive; }
  [[nodiscard]] bool dismissOnOutsideClick() const override { return false; }
  [[nodiscard]] InputArea* initialFocusArea() const override;
  [[nodiscard]] bool handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) override;

private:
  void onPanelCardOpacityChanged(float opacity) override;
  void doLayout(Renderer& renderer, float width, float height) override;
  void doUpdate(Renderer& renderer) override;
  void submit();
  void cancel();

  ConfigService* m_config = nullptr;
  okaru::auth::AuthPromptService* m_service = nullptr;
  std::string m_shownId; // solicitud que muestra el diálogo; al cerrarlo sin responder se cancela
  bool m_revealed = false;

  Flex* m_rootLayout = nullptr;
  Label* m_titleLabel = nullptr;
  Label* m_messageLabel = nullptr;
  Label* m_requesterLabel = nullptr;
  Label* m_promptLabel = nullptr;
  Label* m_errorLabel = nullptr;
  Input* m_input = nullptr;
  Button* m_revealButton = nullptr;
  Button* m_submitButton = nullptr;
};
