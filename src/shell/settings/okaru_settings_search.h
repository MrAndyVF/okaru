#pragma once

// OKARU: búsqueda en las páginas propias de OKARU (Monitores, Workspaces, Ventanas, Inicio, Entorno…),
// cuyas opciones no están en el registro de settings de OKARU. Cada resultado lleva a su página.

#include <string>
#include <string_view>
#include <vector>

namespace settings {

  struct OkaruSearchItem {
    std::string section;  // id de la sección a la que lleva
    std::string label;    // texto que ve el usuario (ya traducido)
    std::string detail;   // contexto (grupo o descripción)
    std::string keywords; // palabras extra (nombre interno en inglés), para buscar en otro idioma
  };

  // Todas las palabras de `query` aparecen en la etiqueta, el detalle o las palabras clave (sin mayúsculas).
  [[nodiscard]] bool okaruSearchMatches(const OkaruSearchItem& item, std::string_view query);

  // Índice de las páginas propias, en el idioma actual.
  [[nodiscard]] std::vector<OkaruSearchItem> okaruSearchIndex();
  [[nodiscard]] std::vector<OkaruSearchItem> okaruSearch(std::string_view query);

  // Opciones de Ventanas, Teclado y ratón y Comportamiento (las define okaru_settings_windows.cpp).
  [[nodiscard]] std::vector<OkaruSearchItem> okaruWindowsSearchItems();

} // namespace settings
