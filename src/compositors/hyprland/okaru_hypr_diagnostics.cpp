#include "compositors/hyprland/okaru_hypr_diagnostics.h"

#include "compositors/hyprland/hyprland_runtime.h"
#include "compositors/hyprland/okaru_hypr_options.h"

#include <algorithm>
#include <ctime>
#include <format>
#include <fstream>
#include <regex>

namespace okaru::hypr {

  namespace {

    constexpr std::string_view kBak = ".bak-";

    std::optional<Backup> backupFor(const std::filesystem::path& path, const std::filesystem::path& configDir) {
      const std::string name = path.filename().string();
      const auto pos = name.rfind(kBak);
      if (pos == std::string::npos || pos == 0 || pos + kBak.size() >= name.size()) {
        return std::nullopt;
      }
      std::error_code ec;
      Backup backup{
          .path = path,
          .target = path.parent_path() / name.substr(0, pos),
          .label = std::filesystem::relative(path, configDir, ec).string(),
          .stamp = name.substr(pos + kBak.size()),
          .directory = std::filesystem::is_directory(path, ec),
      };
      if (ec || backup.label.empty()) {
        backup.label = path.string();
      }
      return backup;
    }

  } // namespace

  std::optional<HyprVersion> parseVersionJson(const nlohmann::json& json) {
    if (!json.is_object() || !json.contains("version")) {
      return std::nullopt;
    }
    std::string commit = json.value("commit", std::string());
    return HyprVersion{.version = json.value("version", std::string()), .commit = commit.substr(0, 7)};
  }

  std::vector<std::string> parseConfigErrors(const nlohmann::json& json) {
    std::vector<std::string> out;
    if (!json.is_array()) {
      return out;
    }
    for (const auto& item : json) {
      if (item.is_string() && !item.get<std::string>().empty()) {
        out.push_back(item.get<std::string>()); // Hyprland devuelve [""] cuando no hay errores
      }
    }
    return out;
  }

  std::optional<HyprVersion> queryVersion(compositors::hyprland::HyprlandRuntime& runtime) {
    const auto json = runtime.requestJson("j/version");
    return json.has_value() ? parseVersionJson(*json) : std::nullopt;
  }

  std::vector<std::string> queryConfigErrors(compositors::hyprland::HyprlandRuntime& runtime) {
    const auto json = runtime.requestJson("j/configerrors");
    return json.has_value() ? parseConfigErrors(*json) : std::vector<std::string>{};
  }

  std::vector<Backup> findBackups(const std::filesystem::path& configDir) {
    std::vector<Backup> out;
    std::error_code ec;
    // Carpetas completas: ~/.config/hypr.bak-*
    for (const auto& entry : std::filesystem::directory_iterator(configDir, ec)) {
      if (entry.is_directory(ec) && entry.path().filename().string().starts_with("hypr.bak-")) {
        if (auto b = backupFor(entry.path(), configDir)) {
          out.push_back(std::move(*b));
        }
      }
    }
    // Archivos sueltos en hypr/ y hypr/oka/
    for (const auto& dir : {configDir / "hypr", configDir / "hypr" / "oka"}) {
      for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec) && entry.path().filename().string().find(kBak) != std::string::npos) {
          if (auto b = backupFor(entry.path(), configDir)) {
            out.push_back(std::move(*b));
          }
        }
      }
    }
    std::ranges::sort(out, [](const Backup& a, const Backup& b) {
      return a.stamp != b.stamp ? a.stamp > b.stamp : a.label < b.label;
    });
    return out;
  }

  std::vector<Backup> findBackups() {
    return findBackups(okaFilePath("layout").parent_path().parent_path().parent_path());
  }

  std::optional<std::string> restoreBackup(const Backup& backup, std::string_view stamp) {
    std::error_code ec;
    if (!std::filesystem::exists(backup.path, ec)) {
      return "ya no existe " + backup.path.string();
    }
    const auto safety =
        backup.target.parent_path() / std::format("{}{}okaru-{}", backup.target.filename().string(), kBak, stamp);
    if (std::filesystem::exists(safety, ec)) {
      return "ya existe " + safety.string();
    }
    if (backup.directory) {
      // Carpeta: lo actual se mueve a un lado (no se borra) y la copia toma su lugar.
      if (std::filesystem::exists(backup.target, ec)) {
        std::filesystem::rename(backup.target, safety, ec);
        if (ec) {
          return "no se pudo respaldar " + backup.target.string() + ": " + ec.message();
        }
      }
      std::filesystem::copy(backup.path, backup.target, std::filesystem::copy_options::recursive, ec);
      if (ec) {
        std::error_code undo;
        std::filesystem::remove_all(backup.target, undo);
        std::filesystem::rename(safety, backup.target, undo);
        return "no se pudo copiar " + backup.path.string() + ": " + ec.message();
      }
      return std::nullopt;
    }
    if (std::filesystem::exists(backup.target, ec)) {
      std::filesystem::copy_file(backup.target, safety, ec);
      if (ec) {
        return "no se pudo respaldar " + backup.target.string() + ": " + ec.message();
      }
    }
    std::filesystem::copy_file(backup.path, backup.target, std::filesystem::copy_options::overwrite_existing, ec);
    return ec ? std::optional("no se pudo restaurar " + backup.target.string() + ": " + ec.message()) : std::nullopt;
  }

  std::optional<std::string> restoreBackup(const Backup& backup) {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
    return restoreBackup(backup, stamp);
  }

  std::vector<ManagedBlock> findManagedBlocks(const std::filesystem::path& okaDir) {
    static const std::regex kBegin(R"(^\s*-- okaru:([a-z-]+):begin)");
    std::vector<ManagedBlock> out;
    std::error_code ec;
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(okaDir, ec)) {
      if (entry.path().extension() == ".lua") {
        files.push_back(entry.path());
      }
    }
    std::ranges::sort(files);
    for (const auto& path : files) {
      std::ifstream file(path);
      std::string line;
      while (std::getline(file, line)) {
        std::smatch m;
        if (std::regex_search(line, m, kBegin)) {
          out.push_back({.file = path.filename().string(), .block = m[1].str()});
        }
      }
    }
    return out;
  }

} // namespace okaru::hypr
