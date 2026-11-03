#include "compositors/hyprland/okaru_hypr_workspaces.h"

#include "compositors/hyprland/hyprland_runtime.h"
#include "compositors/hyprland/okaru_hypr_outputs.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>
#include <regex>
#include <sstream>

namespace okaru::hypr {

  namespace {

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

    std::string unescape(std::string_view text) {
      std::string out;
      for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < text.size()) {
          ++i;
        }
        out += text[i];
      }
      return out;
    }

    std::string readFile(const std::filesystem::path& path) {
      std::ifstream in(path, std::ios::binary);
      std::stringstream buffer;
      buffer << in.rdbuf();
      return buffer.str();
    }

  } // namespace

  bool isValidWorkspaceName(std::string_view name) {
    return !name.empty()
        && name.size() <= 64
        && name.find('\n') == std::string_view::npos
        && name.find('\r') == std::string_view::npos;
  }

  bool isValidMonitorName(std::string_view name) {
    return !name.empty()
        && name.size() < 64
        && std::ranges::all_of(name, [](unsigned char c) { return std::isalnum(c) != 0 || c == '-' || c == '_'; });
  }

  std::vector<WorkspaceDef> parseWorkspaces(std::string_view lua) {
    const std::string text(lua);
    const auto begin = text.find(kWorkspacesBegin);
    const auto end = begin == std::string::npos ? std::string::npos : text.find(kWorkspacesEnd, begin);
    if (begin == std::string::npos || end == std::string::npos) {
      return {};
    }
    const std::string block = text.substr(begin, end - begin);
    static const std::regex kRule(R"re(hl\.workspace_rule\s*\(\s*\{([^}]*)\}\s*\))re");
    static const std::regex kField(R"re((\w+)\s*=\s*(?:"((?:[^"\\]|\\.)*)"|(true|false)))re");
    std::vector<WorkspaceDef> out;
    for (std::sregex_iterator it(block.begin(), block.end(), kRule), stop; it != stop; ++it) {
      const std::string body = (*it)[1].str();
      WorkspaceDef def;
      for (std::sregex_iterator f(body.begin(), body.end(), kField); f != stop; ++f) {
        const std::string key = (*f)[1].str();
        const std::string str = unescape((*f)[2].str());
        const bool boolean = (*f)[3].str() == "true";
        if (key == "workspace") {
          def.id = std::atoi(str.c_str());
        } else if (key == "monitor") {
          def.monitor = str;
        } else if (key == "default_name") {
          def.name = str;
        } else if (key == "persistent") {
          def.persistent = boolean;
        } else if (key == "default") {
          def.isDefault = boolean;
        }
      }
      if (def.id > 0) {
        out.push_back(std::move(def));
      }
    }
    return out;
  }

  std::string serializeWorkspacesBlock(const std::vector<WorkspaceDef>& defs, std::string_view primary) {
    std::vector<WorkspaceDef> sorted = defs;
    std::ranges::sort(sorted, [](const WorkspaceDef& a, const WorkspaceDef& b) {
      return a.monitor != b.monitor ? a.monitor < b.monitor : a.id < b.id;
    });
    std::vector<const WorkspaceDef*> primaryDefs;
    for (const auto& def : sorted) {
      if (def.monitor == primary) {
        primaryDefs.push_back(&def);
      }
    }
    std::ranges::sort(primaryDefs, {}, &WorkspaceDef::id);

    std::string out(kWorkspacesBegin);
    out += " (lo escribe OKARU → Workspaces; los cambios a mano dentro del bloque se pierden)\n";
    out += "local names = {";
    for (std::size_t i = 0; i < primaryDefs.size(); ++i) {
      out += (i == 0 ? " " : ", ") + luaString(primaryDefs[i]->name);
    }
    out += primaryDefs.empty() ? "}" : " }";
    out += " -- monitor principal; lo usa user-binds.lua\n";
    for (const auto& def : sorted) {
      out += std::format(
          "hl.workspace_rule({{ workspace = \"{}\", monitor = {}, default_name = {}, persistent = {}, default = {} "
          "}})\n",
          def.id, luaString(def.monitor), luaString(def.name), def.persistent ? "true" : "false",
          def.isDefault ? "true" : "false"
      );
    }
    out += kWorkspacesEnd;
    out += "\n";
    return out;
  }

  std::string replaceWorkspacesBlock(std::string_view fileText, std::string_view block) {
    std::string text(fileText);
    const auto begin = text.find(kWorkspacesBegin);
    if (begin != std::string::npos) {
      const auto endMarker = text.find(kWorkspacesEnd, begin);
      if (endMarker != std::string::npos) {
        auto end = endMarker + kWorkspacesEnd.size();
        if (end < text.size() && text[end] == '\n') {
          ++end;
        }
        const auto lineStart = text.rfind('\n', begin);
        const auto start = lineStart == std::string::npos ? 0 : lineStart + 1;
        return text.substr(0, start) + std::string(block) + text.substr(end);
      }
    }
    // Primera vez: antes del `return` final (en Lua debe ser la última sentencia).
    static const std::regex kReturn(R"((^|\n)return\b)");
    std::smatch m;
    std::string::size_type insertAt = text.size();
    for (auto it = std::sregex_iterator(text.begin(), text.end(), kReturn); it != std::sregex_iterator(); ++it) {
      m = *it;
      insertAt = static_cast<std::string::size_type>(m.position(0)) + (m[1].length() > 0 ? 1 : 0);
    }
    if (insertAt == text.size() && !text.empty() && !text.ends_with('\n')) {
      text += '\n';
      insertAt = text.size();
    }
    return text.substr(0, insertAt) + std::string(block) + "\n" + text.substr(insertAt);
  }

  int nextWorkspaceId(const std::vector<WorkspaceDef>& defs, std::string_view monitor) {
    int maxOwn = 0;
    int maxAll = 0;
    for (const auto& def : defs) {
      maxAll = std::max(maxAll, def.id);
      if (def.monitor == monitor) {
        maxOwn = std::max(maxOwn, def.id);
      }
    }
    int candidate = maxOwn > 0 ? maxOwn + 1 : (maxAll / 10 + 1) * 10 + 1;
    while (std::ranges::any_of(defs, [&](const WorkspaceDef& d) { return d.id == candidate; })) {
      ++candidate;
    }
    return candidate;
  }

  std::vector<WorkspaceDef> readWorkspaces() { return parseWorkspaces(readFile(outputsFilePath())); }

  std::optional<std::string> writeWorkspaces(const std::vector<WorkspaceDef>& defs, std::string_view primary) {
    for (const auto& def : defs) {
      if (!isValidWorkspaceName(def.name) || !isValidMonitorName(def.monitor) || def.id <= 0) {
        return "workspace no válido";
      }
    }
    const auto path = outputsFilePath();
    const std::string current = readFile(path);
    const std::string next = replaceWorkspacesBlock(current, serializeWorkspacesBlock(defs, primary));
    if (next == current) {
      return std::nullopt;
    }
    const auto tmp = path.string() + ".okaru-tmp";
    {
      std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
      if (!out || !(out << next)) {
        return "no se pudo escribir " + tmp;
      }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    return ec ? std::optional(ec.message()) : std::nullopt;
  }

  std::optional<std::string>
  applyWorkspacesLive(compositors::hyprland::HyprlandRuntime& runtime, const std::vector<WorkspaceDef>& defs) {
    std::string code;
    for (const auto& def : defs) {
      code += std::format(
          "hl.workspace_rule({{ workspace = \"{}\", monitor = {}, default_name = {}, persistent = {}, default = {} "
          "}})\n",
          def.id, luaString(def.monitor), luaString(def.name), def.persistent ? "true" : "false",
          def.isDefault ? "true" : "false"
      );
      code += std::format(
          "hl.dispatch(hl.dsp.workspace.rename({{ workspace = {}, name = {} }}))\n", def.id, luaString(def.name)
      );
    }
    const auto response = runtime.request("eval " + code);
    if (!response.has_value()) {
      return "Hyprland no respondió";
    }
    if (response->starts_with("ok") || response->empty()) {
      return std::nullopt;
    }
    return *response;
  }

  std::optional<std::string>
  renameWorkspace(compositors::hyprland::HyprlandRuntime& runtime, int id, std::string_view name) {
    if (!isValidWorkspaceName(name)) {
      return "nombre no válido";
    }
    auto defs = readWorkspaces();
    const auto it = std::ranges::find(defs, id, &WorkspaceDef::id);
    if (it == defs.end()) {
      return "el workspace no está en el bloque okaru:workspaces";
    }
    it->name = std::string(name);
    std::string primary = readFilePrimary();
    if (primary.empty()) {
      primary = defs.front().monitor;
    }
    if (auto err = writeWorkspaces(defs, primary)) {
      return err;
    }
    return applyWorkspacesLive(runtime, defs);
  }

} // namespace okaru::hypr
