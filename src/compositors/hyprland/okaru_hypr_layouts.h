#pragma once

// OKARU: layout de Hyprland por workspace o global (Super+Alt+Space, widget hypr_layout).
// Por workspace se guarda como hl.workspace_rule en el bloque okaru:layouts al final de
// hypr/oka/layout.lua; global, como general:layout en el bloque okaru:options. Así sobrevive a
// reinicios en los dos casos.

#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace compositors::hyprland {
  class HyprlandRuntime;
}

namespace okaru::hypr {

  using WorkspaceLayouts = std::map<std::string, std::string>; // workspace -> layout

  // Layouts propios de Hyprland, en el orden del ciclo.
  [[nodiscard]] bool isKnownLayout(std::string_view layout);
  // `target` = "next", "prev" o el nombre de un layout. nullopt si no es válido.
  [[nodiscard]] std::optional<std::string> resolveLayout(std::string_view current, std::string_view target);

  [[nodiscard]] std::string serializeLayoutsBlock(const WorkspaceLayouts& layouts);
  [[nodiscard]] WorkspaceLayouts parseLayoutsBlock(std::string_view lua);
  // Con `layouts` vacío quita el bloque.
  [[nodiscard]] std::string replaceLayoutsBlock(std::string_view fileText, const WorkspaceLayouts& layouts);

  // Layout del workspace activo según Hyprland ("" si no responde).
  [[nodiscard]] std::string activeLayout(compositors::hyprland::HyprlandRuntime& runtime);
  // Layout del workspace visible en el monitor `output` (conector, p. ej. "DP-1"); sin monitor, el activo.
  [[nodiscard]] std::string outputLayout(compositors::hyprland::HyprlandRuntime& runtime, std::string_view output);
  // Icono Tabler de cada layout ("layout-grid" si es desconocido).
  [[nodiscard]] std::string_view layoutGlyph(std::string_view layout);

  struct LayoutResult {
    std::string layout;
    std::string error; // vacío si todo salió bien
  };

  // Cambia el layout del workspace activo (perWorkspace) o el de todo el sistema, en vivo y en
  // layout.lua. En modo global los layouts propios de cada workspace se borran: todos siguen al nuevo.
  [[nodiscard]] LayoutResult
  setLayout(compositors::hyprland::HyprlandRuntime& runtime, std::string_view target, bool perWorkspace);

} // namespace okaru::hypr
