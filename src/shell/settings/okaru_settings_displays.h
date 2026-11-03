#pragma once

// OKARU: página Pantallas (Hyprland). Vista previa con arrastrar y soltar, todas las opciones de
// hl.monitor, aplicación en vivo con confirmación y persistencia en hypr/oka/outputs.lua.

#include <functional>

class Flex;

namespace settings {

  struct SettingsDisplaysContext {
    float scale = 1.0F;
    // Vuelve a dibujar el contenido de settings. Debe no hacer nada si la ventana está cerrada.
    std::function<void()> requestRebuild;
  };

  void addSettingsDisplays(Flex& content, SettingsDisplaysContext ctx);

} // namespace settings
