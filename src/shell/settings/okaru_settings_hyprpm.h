#pragma once

// OKARU: Extensiones → Plugins de Hyprland. Repos y plugins de hyprpm; cada acción es elección del
// usuario (agregar un repo no activa sus plugins) y pide la contraseña con el diálogo de OKARU.

#include <functional>

class Flex;

namespace settings {

  struct SettingsHyprpmContext {
    float scale = 1.0F;
    std::function<void()> requestRebuild;
  };

  void addSettingsHyprpm(Flex& content, SettingsHyprpmContext ctx);

} // namespace settings
