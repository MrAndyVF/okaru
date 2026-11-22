#pragma once

// OKARU: la vista de atajos de okaru-launcher. No lista todos los atajos: un catálogo curado de las funciones de
// fábrica (apps, OKARU, ventanas, capturas, sesión) con descripciones breves, cada una con la tecla que el usuario
// tenga en su binds.lua (se reconoce por lo que hace el atajo). Más los atajos propios que lleven `description`.

#include "compositors/hyprland/okaru_hypr_binds.h"

#include <string>
#include <string_view>
#include <vector>

namespace okaru::launcher {

  struct KeybindItem {
    std::string group;       // id del grupo: apps, okaru, windows, screenshots, session, custom
    std::string description; // id del catálogo (se traduce) o la description del usuario si group == "custom"
    std::vector<std::string> keys;
  };

  // "SUPER + SHIFT + Return" → {"Super", "Shift", "↵"}.
  [[nodiscard]] std::vector<std::string> keyNames(std::string_view keys);

  // Catálogo aplicado a los atajos reales, en el orden del catálogo (y al final los propios con descripción).
  [[nodiscard]] std::vector<KeybindItem> curatedKeybinds(const std::vector<okaru::hypr::Bind>& binds);

} // namespace okaru::launcher
