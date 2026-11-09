#pragma once

// OKARU: paleta que la shell comparte con sus apps (okaru-calc, …). La shell escribe la paleta resuelta del modo
// activo en ~/.local/state/okaru/palette.json cada vez que cambia; las apps la leen al arrancar y vigilan el archivo.

#include "theme/fixed_palette.h"

#include <filesystem>
#include <optional>

namespace okaru::theme {

  struct AppsPalette {
    TokenMap tokens;
    bool light = false;
  };

  [[nodiscard]] std::filesystem::path appsPalettePath();
  // Escritura atómica: una app nunca lee un archivo a medias.
  bool writeAppsPalette(const AppsPalette& palette);
  [[nodiscard]] std::optional<AppsPalette> readAppsPalette(const std::filesystem::path& path = appsPalettePath());

} // namespace okaru::theme
