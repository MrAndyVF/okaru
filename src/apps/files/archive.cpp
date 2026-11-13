#include "apps/files/archive.h"

#include "apps/files/file_model.h"
#include "core/process/process.h"

#include <algorithm>
#include <array>
#include <filesystem>

namespace okaru::files {

  namespace {

    namespace fs = std::filesystem;

    constexpr std::array<const char*, 12> kExtensions{".tar.gz", ".tar.xz", ".tar.zst", ".tar.bz2", ".tgz", ".txz",
                                                      ".tar",    ".zip",    ".7z",       ".rar",     ".iso", ".cpio"};

    // Nombre libre en la carpeta (ruta local).
    fs::path freePath(const fs::path& folder, const std::string& name) {
      return folder / uniqueName(pathToUri(folder.string()), name);
    }

  } // namespace

  bool isArchive(const std::string& type, const std::string& name) {
    static constexpr std::array<const char*, 14> kTypes{
        "application/zip", "application/x-zip-compressed", "application/x-tar", "application/x-compressed-tar",
        "application/x-xz-compressed-tar", "application/x-zstd-compressed-tar", "application/x-bzip2-compressed-tar",
        "application/x-7z-compressed", "application/vnd.rar", "application/x-rar", "application/x-rar-compressed",
        "application/x-cd-image", "application/x-iso9660-image", "application/x-cpio"};
    for (const char* t : kTypes) {
      if (type == t) {
        return true;
      }
    }
    return std::ranges::any_of(kExtensions, [&](const char* ext) { return name.ends_with(ext); });
  }

  std::string archiveStem(const std::string& name) {
    for (const char* ext : kExtensions) {
      if (name.ends_with(ext) && name.size() > std::string_view(ext).size()) {
        return name.substr(0, name.size() - std::string_view(ext).size());
      }
    }
    return name;
  }

  bool compressFiles(const std::string& folder, const std::vector<std::string>& names, const std::string& output,
                     std::string& error) {
    // -a: el formato sale de la extensión del archivo de salida.
    std::vector<std::string> args{"bsdtar", "-a", "-cf", output, "-C", folder, "--"};
    args.insert(args.end(), names.begin(), names.end());
    const auto result = process::runSyncWithTimeout(args, std::chrono::hours(2));
    if (!result) {
      error = result.err.empty() ? result.out : result.err;
      std::error_code ec;
      fs::remove(output, ec); // no dejar un comprimido a medias
      return false;
    }
    return true;
  }

  bool extractArchive(const std::string& archive, const std::string& folder, std::string& result, std::string& error) {
    const fs::path target = freePath(folder, archiveStem(fs::path(archive).filename().string()));
    std::error_code ec;
    fs::create_directories(target, ec);
    const auto run = process::runSyncWithTimeout({"bsdtar", "-xf", archive, "-C", target.string()}, std::chrono::hours(2));
    if (!run) {
      error = run.err.empty() ? run.out : run.err;
      fs::remove_all(target, ec);
      return false;
    }
    // Una sola cosa adentro (lo normal: "proyecto.zip" con la carpeta "proyecto/"): sin carpeta doble.
    std::vector<fs::path> inside;
    for (const auto& item : fs::directory_iterator(target, ec)) {
      inside.push_back(item.path());
    }
    if (inside.size() == 1) {
      // La carpeta temporal se aparta antes: lo de adentro suele llamarse igual que ella.
      const fs::path temp = freePath(folder, ".okaru-extract");
      fs::rename(target, temp, ec);
      if (!ec) {
        const fs::path single = freePath(folder, inside.front().filename().string());
        fs::rename(temp / inside.front().filename(), single, ec);
        if (!ec) {
          fs::remove(temp, ec);
          result = single.string();
          return true;
        }
        fs::rename(temp, target, ec);
      }
    }
    result = target.string();
    return true;
  }

} // namespace okaru::files
