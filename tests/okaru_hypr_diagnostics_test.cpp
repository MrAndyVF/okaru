// OKARU: diagnóstico de Hyprland (versión, errores, respaldos y su restauración, bloques).
#include "compositors/hyprland/okaru_hypr_diagnostics.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <print>
#include <sstream>
#include <unistd.h>

using namespace okaru::hypr;
namespace fs = std::filesystem;

namespace {

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "okaru_hypr_diagnostics_test: {}", message);
    }
    return condition;
  }

  void write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << text;
  }

  std::string read(const fs::path& path) {
    std::ifstream file(path);
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
  }

} // namespace

int main() {
  bool ok = true;

  const auto version = parseVersionJson(nlohmann::json::parse(R"({"version":"0.57.0","commit":"0aa6745f705f"})"));
  ok = expect(version && version->version == "0.57.0" && version->commit == "0aa6745", "versión") && ok;
  ok = expect(parseConfigErrors(nlohmann::json::parse(R"([""])")).empty(), "[\"\"] es sin errores") && ok;
  ok = expect(parseConfigErrors(nlohmann::json::parse(R"(["a.lua:3: x", ""])")).size() == 1, "un error") && ok;

  const fs::path root = fs::temp_directory_path() / ("okaru-diag-" + std::to_string(::getpid()));
  const fs::path config = root / "config";
  write(config / "hypr/hyprland.lua", "actual\n");
  write(config / "hypr/oka/binds.lua", "binds actual\n");
  write(config / "hypr/oka/binds.lua.bak-2026-11-01", "binds viejo\n");
  write(
      config / "hypr/oka/layout.lua",
      "-- okaru:options:begin\nx\n-- okaru:options:end\n  -- okaru:animations:begin preset=fluid\n"
  );
  write(config / "hypr.bak-2026-10-30/hyprland.lua", "viejo\n");
  write(config / "otra.bak-1/x", "no es de hypr\n");

  const auto backups = findBackups(config);
  ok = expect(backups.size() == 2, "dos respaldos de hypr") && ok;
  ok = expect(backups.size() == 2 && backups[0].stamp == "2026-11-01" && !backups[0].directory, "más nuevo primero")
      && ok;
  ok = expect(backups.size() == 2 && backups[0].target == config / "hypr/oka/binds.lua", "destino del archivo") && ok;
  ok = expect(backups.size() == 2 && backups[1].directory && backups[1].target == config / "hypr", "carpeta") && ok;
  ok =
      expect(backups.size() == 2 && backups[0].label == "hypr/oka/binds.lua.bak-2026-11-01", "etiqueta relativa") && ok;

  // Restaurar un archivo: lo actual queda respaldado y la copia toma su lugar.
  ok = expect(!restoreBackup(backups[0], "T1").has_value(), "restaura archivo") && ok;
  ok = expect(read(config / "hypr/oka/binds.lua") == "binds viejo\n", "contenido restaurado") && ok;
  ok = expect(read(config / "hypr/oka/binds.lua.bak-okaru-T1") == "binds actual\n", "respaldo de lo actual") && ok;
  ok = expect(restoreBackup(backups[0], "T1").has_value(), "no pisa un respaldo de seguridad existente") && ok;

  // Restaurar la carpeta: la actual se mueve a un lado, nada se borra.
  ok = expect(!restoreBackup(backups[1], "T2").has_value(), "restaura carpeta") && ok;
  ok = expect(read(config / "hypr/hyprland.lua") == "viejo\n", "carpeta restaurada") && ok;
  ok = expect(read(config / "hypr.bak-okaru-T2/hyprland.lua") == "actual\n", "carpeta actual a un lado") && ok;
  ok = expect(read(config / "hypr.bak-2026-10-30/hyprland.lua") == "viejo\n", "el respaldo sigue intacto") && ok;

  const auto blocks = findManagedBlocks(config / "hypr.bak-okaru-T2/oka");
  ok = expect(blocks.size() == 2 && blocks[0].block == "options" && blocks[1].block == "animations", "bloques") && ok;
  ok = expect(blocks.size() == 2 && blocks[0].file == "layout.lua", "archivo del bloque") && ok;

  fs::remove_all(root);
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
