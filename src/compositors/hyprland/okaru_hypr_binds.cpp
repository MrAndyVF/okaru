#include "compositors/hyprland/okaru_hypr_binds.h"

#include "compositors/hyprland/hyprland_runtime.h"
#include "compositors/hyprland/okaru_hypr_options.h"
#include "compositors/hyprland/okaru_hypr_rules.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <regex>
#include <sstream>

namespace okaru::hypr {
  namespace {

    // Argumentos de primer nivel de una llamada (entre los paréntesis), respetando strings y anidación.
    std::vector<std::string_view> splitArgs(std::string_view inner) {
      std::vector<std::string_view> args;
      int depth = 0;
      std::size_t start = 0;
      for (std::size_t i = 0; i < inner.size(); ++i) {
        const char c = inner[i];
        if (c == '"' || c == '\'') {
          for (++i; i < inner.size() && inner[i] != c; ++i) {
            if (inner[i] == '\\') {
              ++i;
            }
          }
        } else if (c == '(' || c == '{' || c == '[') {
          ++depth;
        } else if (c == ')' || c == '}' || c == ']') {
          --depth;
        } else if (c == ',' && depth == 0) {
          args.push_back(inner.substr(start, i - start));
          start = i + 1;
        }
      }
      args.push_back(inner.substr(start));
      for (auto& arg : args) {
        while (!arg.empty() && std::isspace(static_cast<unsigned char>(arg.front())) != 0) {
          arg.remove_prefix(1);
        }
        while (!arg.empty() && std::isspace(static_cast<unsigned char>(arg.back())) != 0) {
          arg.remove_suffix(1);
        }
      }
      return args;
    }

    std::optional<std::string> luaStringLiteral(std::string_view text) {
      if (text.starts_with("[[") && text.ends_with("]]") && text.size() >= 4) { // cadena larga de Lua
        return std::string(text.substr(2, text.size() - 4));
      }
      if (text.size() < 2 || (text.front() != '"' && text.front() != '\'') || text.back() != text.front()) {
        return std::nullopt;
      }
      std::string out;
      for (std::size_t i = 1; i + 1 < text.size(); ++i) {
        if (text[i] == '\\' && i + 2 < text.size()) {
          ++i;
        }
        out += text[i];
      }
      return out;
    }

    std::string luaString(std::string_view text) {
      std::string out = "\"";
      for (const char c : text) {
        if (c == '"' || c == '\\') {
          out += '\\';
        }
        out += c;
      }
      return out + "\"";
    }

    std::string lower(std::string_view text) {
      std::string out(text);
      std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return out;
    }

    std::filesystem::path pathFor(BindFile file) {
      return okaFilePath(file == BindFile::Binds ? "binds" : "user-binds");
    }

  } // namespace

  std::vector<Bind> parseBinds(std::string_view text, BindFile file) {
    std::vector<Bind> binds;
    static const std::regex kSection(R"re(^\s*-- === (.+?) ===)re");
    std::string section;
    std::size_t lineBegin = 0;
    std::size_t skipUntil = 0;
    while (lineBegin < text.size()) {
      auto lineEnd = text.find('\n', lineBegin);
      if (lineEnd == std::string_view::npos) {
        lineEnd = text.size();
      }
      const std::string line(text.substr(lineBegin, lineEnd - lineBegin));
      std::smatch m;
      if (std::regex_search(line, m, kSection)) {
        section = m[1].str();
      }
      const auto first = line.find_first_not_of(" \t");
      // Solo `hl.bind(` al inicio de línea: los que están dentro de funciones o bucles llevan sangría
      // o se arman con variables, y esos no se tocan.
      if (lineBegin >= skipUntil && first == 0 && line.starts_with("hl.bind(")) {
        const std::size_t open = lineBegin + 7;
        if (const auto end = luaCallEnd(text, open)) {
          const auto args = splitArgs(text.substr(open + 1, *end - open - 2));
          const auto keys = args.empty() ? std::nullopt : luaStringLiteral(args[0]);
          if (keys && args.size() >= 2 && args.size() <= 3 && !args[1].starts_with("function")) {
            Bind bind{
                .keys = *keys,
                .dispatcher = std::string(args[1]),
                .file = file,
                .section = section,
                .start = lineBegin,
                .end = *end
            };
            bool ok = true;
            if (args.size() == 3) {
              if (auto options = parseLuaTable(args[2]); options && options->is_object()) {
                bind.options = std::move(*options);
              } else {
                ok = false;
              }
            }
            if (ok) {
              binds.push_back(std::move(bind));
            }
          }
          skipUntil = *end;
        }
      }
      lineBegin = lineEnd + 1;
    }
    return binds;
  }

  BindAction actionOf(const Bind& bind) {
    static const std::regex kShell(R"re(^hl\.dsp\.exec_cmd\(\s*ipc\s*\.\.\s*(".*"|\[\[.*\]\])\s*\)$)re");
    static const std::regex kCommand(R"re(^hl\.dsp\.exec_cmd\(\s*(".*"|\[\[.*\]\])\s*\)$)re");
    std::smatch m;
    if (std::regex_match(bind.dispatcher, m, kShell)) {
      if (auto text = luaStringLiteral(m[1].str())) {
        return {.kind = BindAction::Kind::Shell, .text = *text};
      }
    }
    if (std::regex_match(bind.dispatcher, m, kCommand)) {
      if (auto text = luaStringLiteral(m[1].str())) {
        // Un comando que llama a la shell por su nombre también es de la shell.
        for (const std::string_view prefix : {"okaru msg ", "noctalia msg "}) {
          if (text->starts_with(prefix)) {
            return {.kind = BindAction::Kind::Shell, .text = text->substr(prefix.size())};
          }
        }
        return {.kind = BindAction::Kind::Command, .text = *text};
      }
    }
    return {.kind = BindAction::Kind::Other, .text = bind.dispatcher};
  }

  BindCategory categoryOf(const Bind& bind) {
    if (bind.file == BindFile::UserBinds) {
      return BindCategory::User;
    }
    const std::string keys = lower(bind.keys);
    const BindAction action = actionOf(bind);
    const std::string text = lower(action.text);
    const bool mediaKey = keys.find("xf86audio") != std::string::npos
        || keys.find("xf86monbrightness") != std::string::npos
        || keys.find("print") != std::string::npos;
    const bool mediaCommand = std::ranges::any_of(
        std::vector<std::string_view>{
            "volume", "media", "brightness", "mic-", "screenshot", "playerctl", "wpctl", "pactl"
        },
        [&](std::string_view word) { return text.find(word) != std::string::npos; }
    );
    if (mediaKey || (action.kind != BindAction::Kind::Other && mediaCommand)) {
      return BindCategory::Media;
    }
    // La shell y la sesión: paneles, ajustes, cerrar sesión, apagar pantallas.
    if (action.kind == BindAction::Kind::Shell
        || bind.dispatcher.starts_with("hl.dsp.exit(")
        || bind.dispatcher.starts_with("hl.dsp.dpms(")) {
      return BindCategory::System;
    }
    if (action.kind == BindAction::Kind::Command || bind.dispatcher.starts_with("hl.dsp.global(")) {
      return BindCategory::User;
    }
    const bool windowing = std::ranges::any_of(
        std::vector<std::string_view>{
            "hl.dsp.focus", "hl.dsp.window.", "hl.dsp.layout", "hl.dsp.workspace", "hl.dsp.group", "mouse:",
            // Ayudantes de binds.lua que eligen la acción según el layout.
            "focus(", "move(", "by_layout("
        },
        [&](std::string_view word) {
          return bind.dispatcher.find(word) != std::string::npos || keys.find(word) != std::string::npos;
        }
    );
    return windowing ? BindCategory::Input : BindCategory::Other;
  }

  std::string shellDispatcher(std::string_view command) { return "hl.dsp.exec_cmd(ipc .. " + luaString(command) + ")"; }

  std::string commandDispatcher(std::string_view command) { return "hl.dsp.exec_cmd(" + luaString(command) + ")"; }

  std::string serializeBind(std::string_view keys, std::string_view dispatcher, const nlohmann::ordered_json& options) {
    std::string out = "hl.bind(" + luaString(keys) + ", " + std::string(dispatcher);
    if (options.is_object() && !options.empty()) {
      out += ", " + toLuaTable(options);
    }
    return out + ")";
  }

  std::string normalizeKeys(std::string_view keys) {
    std::vector<std::string> parts;
    std::string current;
    for (const char c : keys) {
      if (c == '+') {
        parts.push_back(current);
        current.clear();
      } else if (std::isspace(static_cast<unsigned char>(c)) == 0) {
        current += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      }
    }
    parts.push_back(current);
    std::erase(parts, std::string{});
    if (parts.empty()) {
      return {};
    }
    const std::string key = parts.back();
    parts.pop_back();
    std::ranges::sort(parts);
    std::string out;
    for (const auto& mod : parts) {
      out += mod + "+";
    }
    return out + key;
  }

  const std::vector<BindSection>& bindSections() {
    static const std::vector<BindSection> kSections{
        {"shell", BindCategory::System, {"shell", "noctalia", "okaru"}},
        {"security", BindCategory::System, {"seguridad", "security"}},
        {"session", BindCategory::System, {"sistema", "system", "sesión", "sesion", "session"}},
        {"audio", BindCategory::Media, {"audio", "audio, multimedia y brillo", "sonido", "sound", "volumen", "volume"}},
        {"brightness", BindCategory::Media, {"brillo", "brightness"}},
        {"media", BindCategory::Media, {"multimedia", "media", "reproducción", "playback"}},
        {"screenshots", BindCategory::Media, {"capturas", "screenshots", "capturas de pantalla"}},
        {"windows", BindCategory::Input, {"ventanas", "windows"}},
        {"focus", BindCategory::Input, {"foco", "foco y mover", "focus"}},
        {"layouts", BindCategory::Input, {"layouts", "layout"}},
        {"move", BindCategory::Input, {"mover ventanas / columnas", "mover ventanas", "move windows", "move"}},
        {"columns", BindCategory::Input, {"columnas", "columns"}},
        {"monitors", BindCategory::Input, {"monitores", "monitors"}},
        {"workspaces", BindCategory::Input, {"workspaces", "escritorios"}},
        {"scratchpad", BindCategory::Input, {"scratchpad", "escritorio especial", "special workspace"}},
        {"size", BindCategory::Input, {"tamaño", "size", "resize"}},
        {"apps", BindCategory::User, {"aplicaciones", "aplicaciones de fábrica", "apps", "applications"}},
        {"other", BindCategory::Other, {"otros", "other"}},
    };
    return kSections;
  }

  std::string sectionIdOf(std::string_view header) {
    // "Workspaces (siguiente/anterior ...)" -> "workspaces": lo que va entre paréntesis es un comentario.
    std::string name = lower(header.substr(0, header.find(" (")));
    while (!name.empty() && name.back() == ' ') {
      name.pop_back();
    }
    for (const auto& section : bindSections()) {
      if (std::ranges::contains(section.aliases, name)) {
        return std::string(section.id);
      }
    }
    return {};
  }

  std::string
  insertInSection(std::string_view text, std::string_view sectionId, std::string_view header, std::string_view line) {
    static const std::regex kSection(R"re(^\s*-- === (.+?) ===)re");
    std::string out(text);
    std::size_t lineBegin = 0;
    bool inSection = false;
    std::size_t insertAt = std::string::npos; // tras el encabezado o el último atajo de la sección
    while (lineBegin < out.size()) {
      auto lineEnd = out.find('\n', lineBegin);
      if (lineEnd == std::string::npos) {
        lineEnd = out.size();
      }
      const std::string current = out.substr(lineBegin, lineEnd - lineBegin);
      std::smatch m;
      if (std::regex_search(current, m, kSection)) {
        if (inSection) {
          break; // empieza la siguiente sección
        }
        inSection = sectionIdOf(m[1].str()) == sectionId;
        if (inSection) {
          insertAt = lineEnd;
        }
      } else if (inSection && current.starts_with("hl.bind(")) {
        insertAt = lineEnd; // tras el último atajo de la sección, no tras el código Lua que lo siga
      }
      lineBegin = lineEnd + 1;
    }
    if (insertAt != std::string::npos) {
      return out.insert(insertAt, "\n" + std::string(line));
    }
    if (!out.empty() && !out.ends_with('\n')) {
      out += '\n';
    }
    if (!out.empty()) {
      out += '\n';
    }
    return out + "-- === " + std::string(header) + " ===\n" + std::string(line) + "\n";
  }

  std::string readBindsFile(BindFile file) {
    std::ifstream in(pathFor(file), std::ios::binary);
    if (!in) {
      return {};
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
  }

  std::vector<Bind> readAllBinds() {
    auto binds = parseBinds(readBindsFile(BindFile::Binds), BindFile::Binds);
    auto user = parseBinds(readBindsFile(BindFile::UserBinds), BindFile::UserBinds);
    binds.insert(binds.end(), std::make_move_iterator(user.begin()), std::make_move_iterator(user.end()));
    return binds;
  }

  std::optional<std::string> writeBindFiles(
      compositors::hyprland::HyprlandRuntime& runtime, const std::vector<std::string>& before,
      const std::vector<std::string>& after
  ) {
    const std::vector<BindFile> files{BindFile::Binds, BindFile::UserBinds};
    for (std::size_t i = 0; i < files.size(); ++i) {
      if (before[i] != after[i]) {
        if (auto err = writeFileAtomic(pathFor(files[i]), after[i])) {
          return err;
        }
      }
    }
    (void)runtime.request("reload");
    const auto errors = runtime.request("configerrors");
    if (errors.has_value() && (errors->find("binds.lua") != std::string::npos)) {
      for (std::size_t i = 0; i < files.size(); ++i) {
        if (before[i] != after[i]) {
          (void)writeFileAtomic(pathFor(files[i]), before[i]);
        }
      }
      (void)runtime.request("reload");
      std::string message = *errors;
      while (!message.empty() && (message.back() == '\n' || message.back() == ' ')) {
        message.pop_back();
      }
      return message;
    }
    return std::nullopt;
  }

} // namespace okaru::hypr
