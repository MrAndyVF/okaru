#pragma once

#include "theme/fixed_palette.h"

#include <span>
#include <string_view>

namespace okaru::theme {

  struct FixedPaletteMode {
    FixedPalette palette;
    TerminalPalette terminal;
  };

  struct BuiltinPalette {
    std::string_view name;
    FixedPaletteMode dark;
    FixedPaletteMode light;
  };

  std::span<const BuiltinPalette> builtinPalettes();

  const BuiltinPalette* findBuiltinPalette(std::string_view name);
  GeneratedPalette expandBuiltinPalette(const BuiltinPalette& palette);

} // namespace okaru::theme
