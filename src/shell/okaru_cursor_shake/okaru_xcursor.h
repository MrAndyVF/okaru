#pragma once

// OKARU: lee el cursor de flecha del tema Xcursor del usuario (XCURSOR_THEME) en su tamaño más grande, para que el
// cursor agrandado sea el mismo que se ve en pantalla.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace okaru::cursor_shake {

  struct CursorImage {
    int width = 0;
    int height = 0;
    int hotX = 0;
    int hotY = 0;
    int nominalSize = 0;            // tamaño del tema al que corresponde la imagen (24, 48, 144…)
    std::vector<std::uint8_t> rgba; // RGBA8 sin premultiplicar
  };

  // Imagen más grande de un archivo Xcursor (primer cuadro si es animado).
  [[nodiscard]] std::optional<CursorImage> loadXcursorFile(const std::filesystem::path& path);

  // Flecha (left_ptr / default) del tema, siguiendo Inherits= de index.theme.
  [[nodiscard]] std::optional<CursorImage> loadThemeArrow(const std::string& theme);

} // namespace okaru::cursor_shake
