#pragma once

// OKARU: lógica pura del centro de control: catálogo de tarjetas, tamaños permitidos y reparto en filas
// de 4 columnas (pequeña = 1, mediana = 2, grande = 4). Sin UI, para poder probarla.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace okaru::dashboard {

  inline constexpr int kColumns = 4;
  inline constexpr std::string_view kShortcutPrefix = "shortcut:";

  enum class CardSize : std::uint8_t { Small, Medium, Large };

  struct CardKind {
    std::string_view type;  // "clock", "media"… o "shortcut" para todos los accesos directos
    std::string_view glyph; // icono del catálogo y de la vista de edición
    std::vector<CardSize> sizes;
    std::string_view panel; // panel que abre al hacer clic ("" = ninguno)
  };

  [[nodiscard]] int span(CardSize size) noexcept;
  [[nodiscard]] std::optional<CardSize> parseSize(std::string_view name) noexcept;
  [[nodiscard]] std::string_view sizeName(CardSize size) noexcept;
  [[nodiscard]] std::string_view sizeLetter(CardSize size) noexcept;

  // Tarjetas propias en el orden del catálogo de "Agregar" (sin la entrada genérica de acceso directo).
  [[nodiscard]] const std::vector<CardKind>& builtinCardKinds();
  // Tipo de la config → su clase; "shortcut:<id>" → la clase "shortcut". nullptr si no existe.
  [[nodiscard]] const CardKind* findKind(std::string_view type);
  [[nodiscard]] bool isShortcutType(std::string_view type) noexcept;
  [[nodiscard]] std::string_view shortcutId(std::string_view type) noexcept;

  // Tamaño válido para el tipo: el pedido si está permitido, si no el primero permitido.
  [[nodiscard]] CardSize normalizeSize(std::string_view type, std::string_view size);
  // Siguiente tamaño permitido (cíclico).
  [[nodiscard]] CardSize nextSize(std::string_view type, CardSize current);
  // Tamaño al agregar: mediano si se puede, si no el primero permitido.
  [[nodiscard]] CardSize defaultSize(std::string_view type);

  // Reparte las tarjetas en filas, en orden: si una no cabe en la fila actual empieza otra.
  [[nodiscard]] std::vector<std::vector<std::size_t>> packRows(std::span<const CardSize> sizes);

  // Si el centro de control está abierto y aloja ese panel (okaru-network…), lo abre como detalle emergente
  // encima y devuelve true; si no, el llamador abre el panel suelto. Lo registra el centro de control.
  [[nodiscard]] bool openHostedDetail(std::string_view panelId);
  void setHostedDetailOpener(std::function<bool(std::string_view)> opener);

} // namespace okaru::dashboard
