#pragma once

// OKARU: la sección «Comandos» de okaru-launcher: una lista corta y fija que abre partes de la shell (atajos de
// teclado, Ajustes, emojis, portapapeles, sesión y fondos). Se buscan junto con las apps.

#include "launcher/launcher_provider.h"

#include <functional>

class OkaruLauncherCommands final : public LauncherProvider {
public:
  // `showKeybinds` pasa okaru-launcher a la vista de atajos (no cierra el panel).
  explicit OkaruLauncherCommands(std::function<void()> showKeybinds) : m_showKeybinds(std::move(showKeybinds)) {}

  [[nodiscard]] std::string_view defaultPrefix() const override { return ""; }
  [[nodiscard]] std::string_view id() const override { return "Commands"; }
  [[nodiscard]] bool trackUsage() const override { return true; }
  [[nodiscard]] std::vector<LauncherResult> query(std::string_view text) const override;
  bool activate(const LauncherResult& result) override;

private:
  std::function<void()> m_showKeybinds;
};
