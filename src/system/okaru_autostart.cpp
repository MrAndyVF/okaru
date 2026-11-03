#include "system/okaru_autostart.h"

#include "compositors/hyprland/okaru_hypr_options.h"
#include "core/process/process.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>
#include <fstream>
#include <regex>
#include <sstream>

namespace okaru::autostart {

  namespace {

    std::optional<std::string> readFile(const std::filesystem::path& path) {
      std::ifstream file(path, std::ios::binary);
      if (!file) {
        return std::nullopt;
      }
      std::stringstream buffer;
      buffer << file.rdbuf();
      return buffer.str();
    }

    std::optional<std::string> writeFile(const std::filesystem::path& path, const std::string& text) {
      std::error_code ec;
      std::filesystem::create_directories(path.parent_path(), ec);
      const auto tmp = path.string() + ".okaru-tmp";
      {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        if (!file || !(file << text)) {
          return "no se pudo escribir " + tmp;
        }
      }
      std::filesystem::rename(tmp, path, ec);
      return ec ? std::optional("no se pudo guardar " + path.string() + ": " + ec.message()) : std::nullopt;
    }

    std::vector<std::string> splitList(std::string_view value) {
      std::vector<std::string> out;
      std::string item;
      for (const char c : value) {
        if (c == ';' || c == ':') {
          if (!item.empty()) {
            out.push_back(item);
          }
          item.clear();
        } else {
          item += c;
        }
      }
      if (!item.empty()) {
        out.push_back(item);
      }
      return out;
    }

    std::string lower(std::string_view text) {
      std::string out(text);
      std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return out;
    }

    bool isFalse(const Keys& keys, const char* key) {
      const auto it = keys.find(key);
      return it != keys.end() && lower(it->second) == "false";
    }

    bool isTrue(const Keys& keys, const char* key) {
      const auto it = keys.find(key);
      return it != keys.end() && lower(it->second) == "true";
    }

  } // namespace

  Keys parseDesktopEntry(std::string_view text) {
    Keys keys;
    bool inEntry = false;
    std::istringstream in{std::string(text)};
    std::string line;
    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      if (line.starts_with('[')) {
        inEntry = line == "[Desktop Entry]";
        continue;
      }
      const auto eq = line.find('=');
      if (!inEntry || line.starts_with('#') || eq == std::string::npos) {
        continue;
      }
      std::string key = line.substr(0, eq);
      while (!key.empty() && key.back() == ' ') {
        key.pop_back();
      }
      std::string value = line.substr(eq + 1);
      while (!value.empty() && value.front() == ' ') {
        value.erase(0, 1);
      }
      keys.emplace(std::move(key), std::move(value)); // la primera aparición gana
    }
    return keys;
  }

  bool appliesTo(const Keys& keys, std::string_view currentDesktops) {
    const auto desktops = splitList(currentDesktops);
    const auto matches = [&](const char* key) {
      const auto it = keys.find(key);
      if (it == keys.end()) {
        return false;
      }
      return std::ranges::any_of(splitList(it->second), [&](const std::string& d) {
        return std::ranges::any_of(desktops, [&](const std::string& c) { return lower(c) == lower(d); });
      });
    };
    if (keys.contains("OnlyShowIn") && !matches("OnlyShowIn")) {
      return false;
    }
    return !matches("NotShowIn");
  }

  bool isEnabled(const Keys& keys) { return !isTrue(keys, "Hidden") && !isFalse(keys, "X-GNOME-Autostart-enabled"); }

  std::string setKey(std::string_view text, std::string_view key, std::string_view value) {
    std::istringstream in{std::string(text)};
    std::string line;
    std::string out;
    bool inEntry = false;
    bool done = false;
    const std::string prefix = std::string(key) + "=";
    const auto insert = [&] {
      if (!done) {
        out += prefix + std::string(value) + "\n";
        done = true;
      }
    };
    while (std::getline(in, line)) {
      if (line.starts_with('[')) {
        if (inEntry) {
          insert(); // la clave no estaba: va al final de [Desktop Entry]
        }
        inEntry = line.starts_with("[Desktop Entry]");
      } else if (inEntry && !done && line.starts_with(prefix)) {
        out += prefix + std::string(value) + "\n";
        done = true;
        continue;
      }
      out += line + "\n";
    }
    if (inEntry) {
      insert();
    }
    if (!done) {
      out = "[Desktop Entry]\n" + prefix + std::string(value) + "\n" + out;
    }
    return out;
  }

  std::string idForName(std::string_view name) {
    std::string slug;
    for (const unsigned char c : name) {
      if (std::isalnum(c) != 0) {
        slug += static_cast<char>(std::tolower(c));
      } else if (!slug.empty() && slug.back() != '-') {
        slug += '-';
      }
    }
    while (slug.ends_with('-')) {
      slug.pop_back();
    }
    return "okaru-" + (slug.empty() ? std::string("app") : slug) + ".desktop";
  }

  std::string newEntryText(std::string_view name, std::string_view exec, std::string_view icon) {
    std::string text = "[Desktop Entry]\nType=Application\n";
    text += "Name=" + std::string(name) + "\n";
    text += "Exec=" + std::string(exec) + "\n";
    if (!icon.empty()) {
      text += "Icon=" + std::string(icon) + "\n";
    }
    text += "X-OKARU-Created=true\n";
    return text;
  }

  std::filesystem::path userDir() {
    const char* config = std::getenv("XDG_CONFIG_HOME");
    if (config != nullptr && config[0] == '/') {
      return std::filesystem::path(config) / "autostart";
    }
    const char* home = std::getenv("HOME");
    return std::filesystem::path(home != nullptr ? home : "/") / ".config" / "autostart";
  }

  std::vector<std::filesystem::path> systemDirs() {
    const char* dirs = std::getenv("XDG_CONFIG_DIRS");
    std::vector<std::filesystem::path> out;
    for (const auto& dir : splitList(dirs != nullptr && dirs[0] != '\0' ? dirs : "/etc/xdg")) {
      out.push_back(std::filesystem::path(dir) / "autostart");
    }
    return out;
  }

  std::vector<Entry> listEntries(
      const std::filesystem::path& user, const std::vector<std::filesystem::path>& system, std::string_view desktops
  ) {
    std::map<std::string, Entry> byId;
    const auto scan = [&](const std::filesystem::path& dir, bool isUser) {
      std::error_code ec;
      for (const auto& file : std::filesystem::directory_iterator(dir, ec)) {
        if (file.path().extension() != ".desktop") {
          continue;
        }
        const std::string id = file.path().filename().string();
        auto& entry = byId[id];
        if (isUser) {
          entry.user = true;
        } else {
          if (entry.system) {
            continue; // el primer directorio del sistema gana
          }
          entry.system = true;
          if (entry.user) {
            continue; // la copia del usuario ya manda
          }
        }
        const auto text = readFile(file.path());
        if (!text.has_value()) {
          continue;
        }
        const Keys keys = parseDesktopEntry(*text);
        entry.id = id;
        entry.enabled = isEnabled(keys);
        entry.ownedByOkaru = id.starts_with("okaru-") || isTrue(keys, "X-OKARU-Created");
        // Una copia que solo oculta (Hidden=true sin Exec) conserva el nombre del original.
        if (const auto it = keys.find("Name"); it != keys.end()) {
          entry.name = it->second;
        }
        if (const auto it = keys.find("Exec"); it != keys.end()) {
          entry.exec = it->second;
        }
        if (const auto it = keys.find("Icon"); it != keys.end()) {
          entry.icon = it->second;
        }
        if (!appliesTo(keys, desktops)) {
          byId.erase(id);
        }
      }
    };
    scan(user, true);
    for (const auto& dir : system) {
      scan(dir, false);
    }
    std::vector<Entry> out;
    for (auto& [id, entry] : byId) {
      if (entry.name.empty()) {
        entry.name = id.substr(0, id.size() - 8);
      }
      out.push_back(std::move(entry));
    }
    std::ranges::sort(out, [](const Entry& a, const Entry& b) { return lower(a.name) < lower(b.name); });
    return out;
  }

  std::vector<Entry> listEntries() {
    const char* desktops = std::getenv("XDG_CURRENT_DESKTOP");
    return listEntries(userDir(), systemDirs(), desktops != nullptr ? desktops : "Hyprland");
  }

  std::optional<std::string> setEnabled(const Entry& entry, bool enabled) {
    const auto userPath = userDir() / entry.id;
    std::optional<std::string> text = entry.user ? readFile(userPath) : std::nullopt;
    if (!text.has_value()) {
      for (const auto& dir : systemDirs()) {
        if ((text = readFile(dir / entry.id))) {
          break;
        }
      }
    }
    if (!text.has_value()) {
      return "no se encontró " + entry.id;
    }
    std::string updated = setKey(*text, "Hidden", enabled ? "false" : "true");
    if (parseDesktopEntry(updated).contains("X-GNOME-Autostart-enabled")) {
      updated = setKey(updated, "X-GNOME-Autostart-enabled", enabled ? "true" : "false");
    }
    return writeFile(userPath, updated);
  }

  std::optional<std::string> create(std::string_view name, std::string_view exec, std::string_view icon) {
    if (name.empty()
        || exec.empty()
        || name.find('\n') != std::string_view::npos
        || exec.find('\n') != std::string_view::npos) {
      return "nombre o comando vacío";
    }
    const auto path = userDir() / idForName(name);
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
      return "ya existe " + path.filename().string();
    }
    return writeFile(path, newEntryText(name, exec, icon));
  }

  std::optional<std::string> remove(const Entry& entry) {
    if (!entry.ownedByOkaru || !entry.user) {
      return "solo se pueden quitar las entradas que creó OKARU";
    }
    std::error_code ec;
    std::filesystem::remove(userDir() / entry.id, ec);
    return ec ? std::optional(ec.message()) : std::nullopt;
  }

  std::string unitName(std::string_view id) {
    std::string base(id);
    if (base.ends_with(".desktop")) {
      base.resize(base.size() - 8);
    }
    std::string escaped;
    for (std::size_t i = 0; i < base.size(); ++i) {
      const auto c = static_cast<unsigned char>(base[i]);
      if (std::isalnum(c) != 0 || c == ':' || c == '_' || (c == '.' && i > 0)) {
        escaped += static_cast<char>(c);
      } else if (c == '/') {
        escaped += '-';
      } else {
        escaped += std::format("\\x{:02x}", c);
      }
    }
    return "app-" + escaped + "@autostart.service";
  }

  std::map<std::string, bool> parseUnitStates(std::string_view listUnits) {
    std::map<std::string, bool> out;
    std::istringstream in{std::string(listUnits)};
    std::string unit;
    std::string load;
    std::string active;
    std::string rest;
    while (in >> unit >> load >> active) {
      std::getline(in, rest);
      out[unit] = active == "active";
    }
    return out;
  }

  std::map<std::string, bool> queryUnitStates() {
    const auto result = process::runSync(
        std::vector<std::string>{
            "systemctl", "--user", "list-units", "--all", "--plain", "--no-legend", "--no-pager",
            "app-*@autostart.service"
        }
    );
    return result ? parseUnitStates(result.out) : std::map<std::string, bool>{};
  }

  std::vector<HyprlandStartCommand> parseHyprlandStart(std::string_view lua, std::string_view file) {
    static const std::regex kStart(R"re(hl\.on\s*\(\s*"hyprland\.start")re");
    static const std::regex kExec(R"re(hl\.exec_cmd\s*\(\s*"((?:[^"\\]|\\.)*)")re");
    std::vector<HyprlandStartCommand> out;
    std::istringstream in{std::string(lua)};
    std::string line;
    int depth = 0; // >0 dentro de un bloque hyprland.start
    static const std::regex kEnd(R"(\bend\b)");
    const auto capture = [&](const std::string& code) {
      std::smatch m;
      if (std::regex_search(code, m, kExec)) {
        out.push_back({.command = m[1].str(), .file = std::string(file)});
      }
    };
    while (std::getline(in, line)) {
      const auto code = line.substr(0, line.find("--"));
      if (depth == 0) {
        if (std::regex_search(code, kStart)) {
          capture(code);
          depth = std::regex_search(code, kEnd) ? 0 : 1; // en una sola línea se abre y se cierra
        }
        continue;
      }
      capture(code);
      if (code.find("function") != std::string::npos) {
        ++depth;
      }
      if (std::regex_search(code, kEnd)) {
        --depth;
      }
    }
    return out;
  }

  std::vector<HyprlandStartCommand> hyprlandStartCommands() {
    const auto okaDir = okaru::hypr::okaFilePath("layout").parent_path();
    std::vector<std::filesystem::path> files{okaDir.parent_path() / "hyprland.lua"};
    std::error_code ec;
    for (const auto& file : std::filesystem::directory_iterator(okaDir, ec)) {
      if (file.path().extension() == ".lua") {
        files.push_back(file.path());
      }
    }
    std::ranges::sort(files);
    std::vector<HyprlandStartCommand> out;
    for (const auto& path : files) {
      if (const auto text = readFile(path)) {
        auto found = parseHyprlandStart(*text, path.filename().string());
        out.insert(out.end(), found.begin(), found.end());
      }
    }
    return out;
  }

} // namespace okaru::autostart
