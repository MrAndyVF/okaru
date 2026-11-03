#pragma once

// OKARU: Sistema → Inicio (apps al iniciar, que arranca uwsm, y comandos de hyprland.start en solo
// lectura) y Sistema → Entorno (variables de ~/.config/uwsm/env y comportamiento general, misc).

#include <functional>
#include <string_view>

class Flex;

namespace settings {

  struct SettingsStartupContext {
    float scale = 1.0F;
    std::string_view page; // "system-startup" o "system-environment"
    std::function<void()> requestRebuild;
  };

  void addSettingsStartup(Flex& content, SettingsStartupContext ctx);

} // namespace settings
