#include "theme/builtin_palettes.h"

#include "theme/fixed_palette.h"

#include <array>
#include <string_view>

namespace okaru::theme {

  namespace {

    constexpr std::array<BuiltinPalette, 1> kPalettes =
        {
            {
                {
                    .name = "OKARU",
                    // OKARU: azules apagados sobre grises y negro.
                    .dark =
                        FixedPaletteMode{
                            .palette =
                                FixedPalette{
                                    .primary = hex("#7095C8"),
                                    .onPrimary = hex("#0A0C10"),
                                    .secondary = hex("#8796AB"),
                                    .onSecondary = hex("#0A0C10"),
                                    .tertiary = hex("#7FA8C4"),
                                    .onTertiary = hex("#0A0C10"),
                                    .error = hex("#D9707A"),
                                    .onError = hex("#0A0C10"),
                                    .surface = hex("#0B0D11"),
                                    .onSurface = hex("#E6E9EF"),
                                    .surfaceVariant = hex("#161A21"),
                                    .onSurfaceVariant = hex("#9AA4B2"),
                                    .outline = hex("#2A313C"),
                                    .shadow = hex("#000000"),
                                    .hover = hex("#7095C8"),
                                    .onHover = hex("#0A0C10"),
                                },
                            .terminal =
                                TerminalPalette{
                                    .normal =
                                        TerminalAnsiColors{
                                            .black = hex("#161A21"),
                                            .red = hex("#D9707A"),
                                            .green = hex("#8BBFA3"),
                                            .yellow = hex("#D4B783"),
                                            .blue = hex("#7095C8"),
                                            .magenta = hex("#A595C9"),
                                            .cyan = hex("#7FA8C4"),
                                            .white = hex("#C9D1DC"),
                                        },
                                    .bright =
                                        TerminalAnsiColors{
                                            .black = hex("#3A4250"),
                                            .red = hex("#E28A93"),
                                            .green = hex("#A6CDB8"),
                                            .yellow = hex("#DFC79E"),
                                            .blue = hex("#93B1D9"),
                                            .magenta = hex("#BCAFD9"),
                                            .cyan = hex("#A3C3D9"),
                                            .white = hex("#F2F4F8"),
                                        },
                                    .foreground = hex("#E6E9EF"),
                                    .background = hex("#0B0D11"),
                                    .selectionFg = hex("#E6E9EF"),
                                    .selectionBg = hex("#2A3546"),
                                    .cursorText = hex("#0B0D11"),
                                    .cursor = hex("#7095C8"),
                                },
                        },
                    .light =
                        FixedPaletteMode{
                            .palette =
                                FixedPalette{
                                    .primary = hex("#3D5F96"),
                                    .onPrimary = hex("#FFFFFF"),
                                    .secondary = hex("#5B6B82"),
                                    .onSecondary = hex("#FFFFFF"),
                                    .tertiary = hex("#3F7596"),
                                    .onTertiary = hex("#FFFFFF"),
                                    .error = hex("#B04A55"),
                                    .onError = hex("#FFFFFF"),
                                    .surface = hex("#F3F5F8"),
                                    .onSurface = hex("#0F1218"),
                                    .surfaceVariant = hex("#E3E7ED"),
                                    .onSurfaceVariant = hex("#4A5363"),
                                    .outline = hex("#B5BDC9"),
                                    .shadow = hex("#C8CED8"),
                                    .hover = hex("#3D5F96"),
                                    .onHover = hex("#FFFFFF"),
                                },
                            .terminal =
                                TerminalPalette{
                                    .normal =
                                        TerminalAnsiColors{
                                            .black = hex("#2A313C"),
                                            .red = hex("#B04A55"),
                                            .green = hex("#2E8B57"),
                                            .yellow = hex("#A86B00"),
                                            .blue = hex("#3D5F96"),
                                            .magenta = hex("#7A4FD6"),
                                            .cyan = hex("#3F7596"),
                                            .white = hex("#4A5363"),
                                        },
                                    .bright =
                                        TerminalAnsiColors{
                                            .black = hex("#6B7585"),
                                            .red = hex("#C2626C"),
                                            .green = hex("#3BA56C"),
                                            .yellow = hex("#C98A10"),
                                            .blue = hex("#557BB3"),
                                            .magenta = hex("#9168E8"),
                                            .cyan = hex("#5A8FB0"),
                                            .white = hex("#0F1218"),
                                        },
                                    .foreground = hex("#0F1218"),
                                    .background = hex("#F3F5F8"),
                                    .selectionFg = hex("#0F1218"),
                                    .selectionBg = hex("#D3DCEA"),
                                    .cursorText = hex("#F3F5F8"),
                                    .cursor = hex("#3D5F96"),
                                },
                        },
                },
            }
    };

  } // namespace

  std::span<const BuiltinPalette> builtinPalettes() { return kPalettes; }

  const BuiltinPalette* findBuiltinPalette(std::string_view name) {
    for (const auto& palette : kPalettes) {
      if (palette.name == name) {
        return &palette;
      }
    }
    return nullptr;
  }

  GeneratedPalette expandBuiltinPalette(const BuiltinPalette& palette) {
    auto generated = expandFixedPalettes(palette.dark.palette, palette.light.palette);
    applyTerminalPalette(generated.dark, palette.dark.terminal);
    applyTerminalPalette(generated.light, palette.light.terminal);
    return generated;
  }

} // namespace okaru::theme
