#pragma once

// OKARU: Pantallas → Workspaces. Los workspaces viven en el bloque okaru:workspaces de hypr/oka/outputs.lua,
// un hl.workspace_rule por workspace con su nombre limpio. El bloque define `local names` (los del monitor
// principal), que el `return { workspace_names = names }` del archivo expone a user-binds.lua.
// Los iconos no van aquí: los guarda la shell en [widget.workspaces] icons.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace compositors::hyprland {
  class HyprlandRuntime;
}

namespace okaru::hypr {

  inline constexpr std::string_view kWorkspacesBegin = "-- okaru:workspaces:begin";
  inline constexpr std::string_view kWorkspacesEnd = "-- okaru:workspaces:end";

  struct WorkspaceDef {
    int id = 0;
    std::string monitor;
    std::string name;
    bool persistent = true;
    bool isDefault = false;

    bool operator==(const WorkspaceDef&) const = default;
  };

  [[nodiscard]] bool isValidWorkspaceName(std::string_view name);
  [[nodiscard]] bool isValidMonitorName(std::string_view name);

  [[nodiscard]] std::vector<WorkspaceDef> parseWorkspaces(std::string_view lua);
  // `primary`: el monitor cuyos nombres van en `local names` (orden por id).
  [[nodiscard]] std::string serializeWorkspacesBlock(const std::vector<WorkspaceDef>& defs, std::string_view primary);
  // Reemplaza el bloque; si no existe, lo pone antes del `return` final (o al final si no hay).
  [[nodiscard]] std::string replaceWorkspacesBlock(std::string_view fileText, std::string_view block);
  // Siguiente id libre para un monitor: continúa su serie (1-4 → 5; 11-14 → 15).
  [[nodiscard]] int nextWorkspaceId(const std::vector<WorkspaceDef>& defs, std::string_view monitor);

  [[nodiscard]] std::vector<WorkspaceDef> readWorkspaces();
  [[nodiscard]] std::optional<std::string>
  writeWorkspaces(const std::vector<WorkspaceDef>& defs, std::string_view primary);
  // Aplica reglas y nombres en vivo (los workspaces existentes se renombran).
  [[nodiscard]] std::optional<std::string>
  applyWorkspacesLive(compositors::hyprland::HyprlandRuntime& runtime, const std::vector<WorkspaceDef>& defs);

  // Renombra un workspace del bloque (lo guarda y lo aplica en vivo). Error o nullopt.
  [[nodiscard]] std::optional<std::string>
  renameWorkspace(compositors::hyprland::HyprlandRuntime& runtime, int id, std::string_view name);

} // namespace okaru::hypr
