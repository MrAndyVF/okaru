#pragma once

// OKARU: Avanzado → Diagnóstico de Hyprland. Versión, errores de config, respaldos de la config (con
// restauración que antes respalda lo actual) y bloques de hypr/oka/ que administra OKARU.

#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace compositors::hyprland {
  class HyprlandRuntime;
}

namespace okaru::hypr {

  struct HyprVersion {
    std::string version; // "0.57.0"
    std::string commit;  // 7 caracteres
  };

  struct Backup {
    std::filesystem::path path;   // la copia: .../oka/binds.lua.bak-2026-11-01 o ~/.config/hypr.bak-2026-11-01
    std::filesystem::path target; // lo que restaura: .../oka/binds.lua o ~/.config/hypr
    std::string label;            // relativo a ~/.config: "hypr/oka/binds.lua.bak-2026-11-01"
    std::string stamp;            // lo que va después de ".bak-"
    bool directory = false;
  };

  struct ManagedBlock {
    std::string file;  // "layout.lua"
    std::string block; // "options"
  };

  [[nodiscard]] std::optional<HyprVersion> parseVersionJson(const nlohmann::json& json);
  [[nodiscard]] std::vector<std::string> parseConfigErrors(const nlohmann::json& json);
  [[nodiscard]] std::optional<HyprVersion> queryVersion(compositors::hyprland::HyprlandRuntime& runtime);
  [[nodiscard]] std::vector<std::string> queryConfigErrors(compositors::hyprland::HyprlandRuntime& runtime);

  // Copias "<nombre>.bak-<fecha>" en ~/.config (carpetas hypr.bak-*), hypr/ y hypr/oka/. Más nuevas primero.
  [[nodiscard]] std::vector<Backup> findBackups(const std::filesystem::path& configDir);
  [[nodiscard]] std::vector<Backup> findBackups();

  // Restaura `backup` sin borrar nada: lo actual se guarda antes como "<destino>.bak-okaru-<fecha>".
  // Devuelve el error, o nullopt si salió bien.
  [[nodiscard]] std::optional<std::string> restoreBackup(const Backup& backup, std::string_view stamp);
  [[nodiscard]] std::optional<std::string> restoreBackup(const Backup& backup);

  // Marcadores "-- okaru:<bloque>:begin" en los .lua de `okaDir`.
  [[nodiscard]] std::vector<ManagedBlock> findManagedBlocks(const std::filesystem::path& okaDir);

} // namespace okaru::hypr
