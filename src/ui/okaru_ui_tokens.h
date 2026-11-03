#pragma once

// OKARU: los paneles de plugins usan los mismos tokens que la shell. Los tamaños que pide un plugin se
// ajustan al paso más cercano de Style (letra 11/13/14/16/20, espaciado 4/8/12/16); las esquinas son rectas.
// Los widgets de la barra no pasan por aquí.

#include "ui/style.h"

namespace okaru::ui_tokens {

  // 9–11 → 11 (más legible), 12–13 → 13, 14 → 14, 15–17 → 16, 18–24 → 20. Más de 24 se respeta: son
  // dibujos o cifras grandes a propósito (p. ej. el gato, que es una fuente).
  [[nodiscard]] constexpr float snapFontSize(float size) noexcept {
    if (size > 24.0F) {
      return size;
    }
    if (size < 11.5F) {
      return Style::fontSizeMini;
    }
    if (size < 13.5F) {
      return Style::fontSizeCaption;
    }
    if (size < 15.0F) {
      return Style::fontSizeBody;
    }
    if (size < 18.0F) {
      return Style::fontSizeTitle;
    }
    return Style::fontSizeHeader;
  }

  // 0–2 se respetan (líneas de texto pegadas); 3–5 → 4, 6–9 → 8, 10–13 → 12, 14–20 → 16; más de 20 se
  // respeta (separaciones grandes a propósito).
  [[nodiscard]] constexpr float snapSpacing(float value) noexcept {
    if (value <= 2.0F || value > 20.0F) {
      return value;
    }
    if (value < 6.0F) {
      return Style::spaceXs;
    }
    if (value < 10.0F) {
      return Style::spaceSm;
    }
    if (value < 14.0F) {
      return Style::spaceMd;
    }
    return Style::spaceLg;
  }

  static_assert(snapFontSize(9.0F) == 11.0F && snapFontSize(12.0F) == 13.0F && snapFontSize(22.0F) == 20.0F);
  static_assert(snapFontSize(64.0F) == 64.0F);
  static_assert(snapSpacing(1.0F) == 1.0F && snapSpacing(6.0F) == 8.0F && snapSpacing(14.0F) == 16.0F);
  static_assert(snapSpacing(24.0F) == 24.0F);

} // namespace okaru::ui_tokens
