#include "system/okaru_apps.h"

#include "core/deferred_call.h"
#include "core/process/process.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>

namespace okaru::apps {
  namespace {

    namespace fs = std::filesystem;

    constexpr std::string_view kDefaultsGroup = "[Default Applications]";
    constexpr std::string_view kEntryGroup = "[Desktop Entry]";
    constexpr std::string_view kHiddenMark = "X-Okaru-Hidden=true";
    constexpr std::string_view kCopyMark = "X-Okaru-Copy=true";

    std::string env(const char* name, std::string_view fallback) {
      const char* value = std::getenv(name);
      return value != nullptr && *value != '\0' ? std::string(value) : std::string(fallback);
    }

    std::string home() { return env("HOME", "/"); }
    std::string configHome() { return env("XDG_CONFIG_HOME", home() + "/.config"); }
    std::string dataHome() { return env("XDG_DATA_HOME", home() + "/.local/share"); }

    std::vector<std::string> splitDirs(const std::string& value) {
      std::vector<std::string> out;
      std::stringstream stream(value);
      std::string dir;
      while (std::getline(stream, dir, ':')) {
        if (!dir.empty()) {
          out.push_back(dir);
        }
      }
      return out;
    }

    std::vector<std::string> dataDirs() { return splitDirs(env("XDG_DATA_DIRS", "/usr/local/share:/usr/share")); }

    std::string readFile(const fs::path& path) {
      std::ifstream in(path, std::ios::binary);
      if (!in) {
        return {};
      }
      std::ostringstream out;
      out << in.rdbuf();
      return out.str();
    }

    std::optional<std::string> writeFileAtomic(const fs::path& path, std::string_view text) {
      std::error_code ec;
      fs::create_directories(path.parent_path(), ec);
      const fs::path tmp = path.string() + ".okaru-tmp";
      {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
          return "cannot write " + tmp.string();
        }
        out << text;
        if (!out.flush()) {
          return "cannot write " + tmp.string();
        }
      }
      fs::rename(tmp, path, ec);
      if (ec) {
        fs::remove(tmp, ec);
        return "cannot replace " + path.string();
      }
      return std::nullopt;
    }

    std::vector<std::string_view> lines(std::string_view text) {
      std::vector<std::string_view> out;
      std::size_t start = 0;
      while (start < text.size()) {
        const std::size_t end = text.find('\n', start);
        const std::size_t stop = end == std::string_view::npos ? text.size() : end;
        out.push_back(text.substr(start, stop - start));
        start = stop + 1;
      }
      return out;
    }

    bool isGroupHeader(std::string_view line) { return line.starts_with('['); }

    std::string_view keyOf(std::string_view line) {
      const std::size_t eq = line.find('=');
      if (eq == std::string_view::npos) {
        return {};
      }
      std::string_view key = line.substr(0, eq);
      while (!key.empty() && key.back() == ' ') {
        key.remove_suffix(1);
      }
      return key;
    }

    // Orden de la especificación XDG: en cada carpeta, primero <escritorio>-mimeapps.list y luego mimeapps.list.
    std::vector<std::string> mimeappsFiles() {
      std::vector<std::string> desktops;
      for (auto name : splitDirs(env("XDG_CURRENT_DESKTOP", ""))) {
        std::ranges::transform(name, name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        desktops.push_back(std::move(name));
      }
      std::vector<std::string> dirs{configHome()};
      for (const auto& dir : splitDirs(env("XDG_CONFIG_DIRS", "/etc/xdg"))) {
        dirs.push_back(dir);
      }
      dirs.push_back(dataHome() + "/applications");
      for (const auto& dir : dataDirs()) {
        dirs.push_back(dir + "/applications");
      }
      std::vector<std::string> files;
      for (const auto& dir : dirs) {
        for (const auto& desktop : desktops) {
          files.push_back(dir + "/" + desktop + "-mimeapps.list");
        }
        files.push_back(dir + "/mimeapps.list");
      }
      return files;
    }

    // globs2 de todas las carpetas de datos, leído una vez.
    const std::string& allGlobs() {
      static const std::string text = [] {
        std::string out = readFile(dataHome() + "/mime/globs2");
        for (const auto& dir : dataDirs()) {
          out += readFile(dir + "/mime/globs2");
        }
        return out;
      }();
      return text;
    }

    process::RunResult run(const std::vector<std::string>& args) {
      return process::runSync(
          args, process::RunOptions{
                    .timeout = std::chrono::milliseconds(3000),
                    .maxOutputBytes = 64 * 1024,
                    .cancel = nullptr,
                    .env = {{"LC_ALL", "C"}},
                }
      );
    }

    std::string field(std::string_view out, std::string_view name) {
      for (const auto line : lines(out)) {
        std::string_view l = line;
        while (!l.empty() && l.front() == ' ') {
          l.remove_prefix(1);
        }
        if (!l.starts_with(name)) {
          continue;
        }
        const std::size_t colon = l.find(':');
        if (colon == std::string_view::npos) {
          continue;
        }
        std::string_view value = l.substr(colon + 1);
        while (!value.empty() && value.front() == ' ') {
          value.remove_prefix(1);
        }
        return std::string(value);
      }
      return {};
    }

    std::string trimmed(std::string text) {
      while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) {
        text.pop_back();
      }
      return text;
    }

  } // namespace

  std::span<const DefaultCategory> defaultCategories() {
    static const std::vector<DefaultCategory> k{
        {"browser", "world", {"x-scheme-handler/http", "x-scheme-handler/https", "text/html"}, {}},
        {"email", "mail", {"x-scheme-handler/mailto"}, {}},
        {"files", "folder", {"inode/directory"}, {}},
        {"text", "file-text", {"text/plain"}, {}},
        {"images", "photo", {"image/png", "image/jpeg"}, "image/"},
        {"video", "movie", {"video/mp4", "video/x-matroska"}, "video/"},
        {"music", "music", {"audio/mpeg", "audio/flac"}, "audio/"},
        {"pdf", "file-type-pdf", {"application/pdf"}, {}},
        {"archives",
         "file-zip",
         {"application/zip", "application/x-7z-compressed", "application/x-compressed-tar", "application/x-tar",
          "application/vnd.rar", "application/x-rar"},
         {}},
    };
    return k;
  }

  bool handles(const DefaultCategory& category, const DesktopEntry& app) {
    return std::ranges::any_of(app.mimeTypes, [&](const std::string& mime) {
      return std::ranges::contains(category.mimes, std::string_view(mime))
          || (!category.prefix.empty() && mime.starts_with(category.prefix));
    });
  }

  std::vector<const DesktopEntry*> candidatesFor(const DefaultCategory& category) {
    std::vector<const DesktopEntry*> out;
    for (const auto& app : desktopEntries()) {
      if (!app.hidden && handles(category, app)) {
        out.push_back(&app);
      }
    }
    std::ranges::sort(out, {}, [](const DesktopEntry* app) { return app->nameLower; });
    return out;
  }

  std::vector<std::string> mimesToSet(const DefaultCategory& category, const DesktopEntry& app) {
    std::vector<std::string> out;
    for (const auto& mime : app.mimeTypes) {
      const bool wanted = std::ranges::contains(category.mimes, std::string_view(mime))
          || (!category.prefix.empty() && mime.starts_with(category.prefix));
      if (wanted && !std::ranges::contains(out, mime)) {
        out.push_back(mime);
      }
    }
    return out;
  }

  std::string defaultInText(std::string_view text, std::string_view mime) {
    bool inGroup = false;
    for (const auto line : lines(text)) {
      if (isGroupHeader(line)) {
        inGroup = line == kDefaultsGroup;
        continue;
      }
      if (inGroup && keyOf(line) == mime) {
        std::string_view value = line.substr(line.find('=') + 1);
        while (!value.empty() && value.front() == ' ') {
          value.remove_prefix(1);
        }
        return std::string(value.substr(0, value.find(';')));
      }
    }
    return {};
  }

  DefaultsSnapshot loadDefaults() {
    DefaultsSnapshot snapshot;
    for (const auto& file : mimeappsFiles()) {
      if (std::string text = readFile(file); !text.empty()) {
        snapshot.texts.push_back(std::move(text));
      }
    }
    return snapshot;
  }

  std::string DefaultsSnapshot::get(std::string_view mime) const {
    for (const auto& text : texts) {
      if (std::string id = defaultInText(text, mime); !id.empty()) {
        return id;
      }
    }
    return {};
  }

  std::string setDefaultsInText(std::string_view text, const std::vector<std::string>& mimes, std::string_view desktopId) {
    std::vector<std::string> out;
    bool inGroup = false;
    bool sawGroup = false;
    std::size_t groupEnd = 0; // índice en `out` tras la última línea no vacía del grupo
    for (const auto line : lines(text)) {
      if (isGroupHeader(line)) {
        inGroup = line == kDefaultsGroup;
        out.emplace_back(line);
        if (inGroup) {
          sawGroup = true;
          groupEnd = out.size();
        }
        continue;
      }
      if (inGroup && std::ranges::contains(mimes, keyOf(line))) {
        continue; // se reescribe abajo
      }
      out.emplace_back(line);
      if (inGroup && !line.empty()) {
        groupEnd = out.size();
      }
    }
    std::vector<std::string> added;
    if (!desktopId.empty()) {
      for (const auto& mime : mimes) {
        added.push_back(mime + "=" + std::string(desktopId));
      }
    }
    if (!sawGroup) {
      if (!out.empty() && !out.back().empty()) {
        out.emplace_back();
      }
      out.emplace_back(kDefaultsGroup);
      groupEnd = out.size();
    }
    out.insert(out.begin() + static_cast<std::ptrdiff_t>(groupEnd), added.begin(), added.end());
    std::string result;
    for (const auto& line : out) {
      result += line;
      result += '\n';
    }
    return result;
  }

  std::optional<std::string> setDefault(const std::vector<std::string>& mimes, std::string_view desktopId) {
    const fs::path path = configHome() + "/mimeapps.list";
    return writeFileAtomic(path, setDefaultsInText(readFile(path), mimes, desktopId));
  }

  void extensionsFromGlobs(std::string_view globs, std::string_view mime, std::vector<std::string>& out) {
    for (const auto line : lines(globs)) {
      const std::size_t a = line.find(':');
      const std::size_t b = a == std::string_view::npos ? a : line.find(':', a + 1);
      if (b == std::string_view::npos || line.starts_with('#') || line.substr(a + 1, b - a - 1) != mime) {
        continue;
      }
      std::string_view glob = line.substr(b + 1);
      glob = glob.substr(0, glob.find(':'));
      if (glob.starts_with("*.") && glob.find_first_of("*?[", 1) == std::string_view::npos) {
        std::string ext(glob.substr(1));
        if (std::ranges::find(out, ext) == out.end()) {
          out.push_back(std::move(ext));
        }
      }
    }
  }

  std::vector<std::string> extensionsFor(std::string_view mime) {
    static std::map<std::string, std::vector<std::string>, std::less<>> cache;
    if (const auto it = cache.find(mime); it != cache.end()) {
      return it->second;
    }
    std::vector<std::string> out;
    extensionsFromGlobs(allGlobs(), mime, out);
    return cache.emplace(std::string(mime), std::move(out)).first->second;
  }

  std::string hideInText(std::string_view text, bool hidden) {
    std::string result;
    bool inEntry = false;
    for (const auto line : lines(text)) {
      if (isGroupHeader(line)) {
        inEntry = line == kEntryGroup;
        result.append(line);
        result += '\n';
        if (inEntry && hidden) {
          result += "NoDisplay=true\n";
          result += kHiddenMark;
          result += '\n';
        }
        continue;
      }
      if (inEntry && (keyOf(line) == "NoDisplay" || line == kHiddenMark)) {
        continue;
      }
      result.append(line);
      result += '\n';
    }
    return result;
  }

  bool hiddenByOkaru(const DesktopEntry& app) {
    // Solo OKARU escribe la marca, y siempre en la carpeta del usuario: no hace falta leer las del sistema.
    return app.noDisplay && app.path.starts_with(dataHome() + "/applications/") && readFile(app.path).contains(kHiddenMark);
  }

  std::optional<std::string> setHidden(const DesktopEntry& app, bool hidden) {
    const fs::path target = fs::path(dataHome()) / "applications" / app.id;
    const std::string text = readFile(app.path);
    if (text.empty()) {
      return "cannot read " + app.path;
    }
    if (!hidden) {
      if (text.contains(kCopyMark)) {
        std::error_code ec;
        fs::remove(app.path, ec);
        return ec ? std::optional<std::string>("cannot remove " + app.path) : std::nullopt;
      }
      return writeFileAtomic(app.path, hideInText(text, false));
    }
    std::string out = hideInText(text, true);
    if (fs::path(app.path) != target) {
      // Copia en la carpeta del usuario: al mostrarla otra vez se borra entera.
      const std::string mark = std::string(kHiddenMark) + "\n";
      out.insert(out.find(mark) + mark.size(), std::string(kCopyMark) + "\n");
    }
    return writeFileAtomic(target, out);
  }

  std::optional<PackageInfo> packageFor(const DesktopEntry& app) {
    const std::string file = fs::path(app.path).filename().string();
    if (app.origin == DesktopEntryOrigin::Flatpak) {
      PackageInfo info{.manager = PackageManager::Flatpak, .name = file.substr(0, file.rfind(".desktop"))};
      const auto out = run({"flatpak", "info", info.name});
      if (!out) {
        return std::nullopt;
      }
      info.version = field(out.out, "Version:");
      info.size = field(out.out, "Installed:");
      return info;
    }
    if (app.origin == DesktopEntryOrigin::Snap) {
      PackageInfo info{.manager = PackageManager::Snap, .name = file.substr(0, file.find('_'))};
      const auto out = run({"snap", "list", info.name});
      if (!out) {
        return std::nullopt;
      }
      const auto rows = lines(out.out);
      if (rows.size() >= 2) {
        std::istringstream cols{std::string(rows[1])};
        std::string name;
        cols >> name >> info.version;
      }
      return info;
    }
    const auto owner = run({"pacman", "-Qqo", app.path});
    if (!owner) {
      return std::nullopt;
    }
    PackageInfo info{.manager = PackageManager::Pacman, .name = trimmed(owner.out)};
    if (const auto details = run({"pacman", "-Qi", info.name}); details) {
      info.version = field(details.out, "Version");
      info.size = field(details.out, "Installed Size");
    }
    if (run({"pacman", "-Qqm", info.name})) {
      info.manager = PackageManager::Aur;
    }
    return info;
  }

  void uninstall(const PackageInfo& package, std::function<void(std::optional<std::string>)> done) {
    std::vector<std::string> args;
    switch (package.manager) {
    case PackageManager::Pacman:
    case PackageManager::Aur:
      args = {"pkexec", "pacman", "-Rns", "--noconfirm", package.name};
      break;
    case PackageManager::Flatpak:
      args = {"flatpak", "uninstall", "-y", "--noninteractive", package.name};
      break;
    case PackageManager::Snap:
      args = {"pkexec", "snap", "remove", package.name};
      break;
    }
    const bool started = process::runAsync(
        args, process::RunCallbacks{
                  .stdOut = {},
                  .stdErr = {},
                  .onExit =
                      [done](process::RunResult result) {
                        std::optional<std::string> error;
                        if (!result) {
                          error = trimmed(result.err.empty() ? result.out : result.err);
                          if (error->empty()) {
                            error = "exit " + std::to_string(result.exitCode);
                          }
                        }
                        DeferredCall::callLater([done, error] { done(error); });
                      },
              }
    );
    if (!started) {
      done("cannot start " + args.front());
    }
  }

} // namespace okaru::apps
