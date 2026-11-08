#pragma once

// OKARU: página Pantallas (Hyprland y Niri). Vista previa con arrastrar y soltar y aplicación en vivo con
// confirmación. Hyprland: hl.monitor y persistencia en hypr/oka/outputs.lua. Niri: niri/okaru/outputs.kdl.

#include <functional>

class Flex;

namespace settings {

  struct SettingsDisplaysContext {
    float scale = 1.0F;
    // Vuelve a dibujar el contenido de settings. Debe no hacer nada si la ventana está cerrada.
    std::function<void()> requestRebuild;
  };

  void addSettingsDisplays(Flex& content, SettingsDisplaysContext ctx);

  // Niri: si config.kdl aún no carga los archivos de OKARU, dibuja el aviso con el botón Activar y devuelve true.
  bool addNiriIncludeNotice(Flex& content, float scale, const std::function<void()>& rebuild);

} // namespace settings
