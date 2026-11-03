#pragma once

// OKARU: los menús de la barra siempre son flotantes y salen junto al widget que los abrió.
// Cada widget elige cómo se alinea su menú con él (`menu_align` en [widget.<nombre>]).

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace okaru::panel {

  enum class MenuAlign : std::uint8_t { Start, Center, End };

  [[nodiscard]] constexpr MenuAlign menuAlignFromString(std::string_view value) noexcept {
    if (value == "start") {
      return MenuAlign::Start;
    }
    if (value == "end") {
      return MenuAlign::End;
    }
    return MenuAlign::Center;
  }

  // Inicio del menú a lo largo de la barra: Start alinea su borde inicial con el del widget, End el
  // final con el final y Center los centra. Siempre queda dentro de la pantalla, a `padding` del borde.
  [[nodiscard]] constexpr float alignedMenuStart(
      float widgetStart, float widgetEnd, float menuExtent, float outputExtent, float padding, MenuAlign align
  ) noexcept {
    float start = 0.0F;
    switch (align) {
    case MenuAlign::Start:
      start = widgetStart;
      break;
    case MenuAlign::End:
      start = widgetEnd - menuExtent;
      break;
    case MenuAlign::Center:
      start = (widgetStart + widgetEnd - menuExtent) * 0.5F;
      break;
    }
    const float maxStart = std::max(padding, outputExtent - menuExtent - padding);
    return std::clamp(start, padding, maxStart);
  }

} // namespace okaru::panel
