#pragma once

// OKARU: páginas de Ventanas (Hyprland): Distribución y Decoración. Cada cambio se aplica en vivo y
// se guarda como override en el bloque okaru:options al final de hypr/oka/layout.lua.

#include "config/config_types.h"

#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class Flex;

namespace settings {

  struct SettingsWindowsContext {
    float scale = 1.0F;
    std::string_view page; // "window-layout", "window-decoration" o "window-animations"
    std::function<void()> requestRebuild;

    // Animaciones: los estilos también mueven las animaciones de la shell (shell.animation.*).
    bool shellAnimationsEnabled = true;
    double shellAnimationSpeed = 1.0;
    std::string shellAnimationStyle;
    // Distribución: alcance del cambio de layout (shell.layout_per_workspace).
    bool layoutPerWorkspace = true;
    std::function<void(std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>)> setShellOverrides;
  };

  void addSettingsWindows(Flex& content, SettingsWindowsContext ctx);

} // namespace settings
