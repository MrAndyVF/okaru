#pragma once

// OKARU: módulo Home, la portada de los ajustes: versiones (OKARU, Hyprland), sistema, memoria, discos y
// novedades de la versión (cuando haya changelog).

class Flex;
class Renderer;

namespace settings {
  // `renderer` hace falta para el logo de arriba; sin él se omite.
  void addSettingsHome(Flex& content, float scale, Renderer* renderer);
}
