#include "compositors/niri/okaru_niri_config.h"

#include "compositors/niri/niri_runtime.h"
#include "core/process/process.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <format>
#include <fstream>
#include <sstream>

namespace okaru::niri {

  namespace {

    namespace fs = std::filesystem;

    constexpr std::string_view kOutputsInclude = "okaru/outputs.kdl";
    constexpr std::string_view kWorkspacesInclude = "okaru/workspaces.kdl";

    // Mismo orden que wl_output_transform (0-7): nombre en el IPC y en la config.
    constexpr std::array<std::string_view, 8> kIpcTransforms{
        "Normal", "_90", "_180", "_270", "Flipped", "Flipped90", "Flipped180", "Flipped270",
    };
    constexpr std::array<std::string_view, 8> kConfigTransforms{
        "normal", "90", "180", "270", "flipped", "flipped-90", "flipped-180", "flipped-270",
    };

    std::string_view trim(std::string_view text) {
      while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) {
        text.remove_prefix(1);
      }
      while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
      }
      return text;
    }

    std::vector<std::string_view> lines(std::string_view text) {
      std::vector<std::string_view> out;
      while (!text.empty()) {
        const auto end = text.find('\n');
        out.push_back(trim(text.substr(0, end)));
        if (end == std::string_view::npos) {
          break;
        }
        text.remove_prefix(end + 1);
      }
      return out;
    }

    // Primera cadena entre comillas de una línea, con los escapes \" y \\ resueltos.
    std::optional<std::string> quoted(std::string_view line) {
      const auto open = line.find('"');
      if (open == std::string_view::npos) {
        return std::nullopt;
      }
      std::string value;
      for (std::size_t i = open + 1; i < line.size(); ++i) {
        if (line[i] == '\\' && i + 1 < line.size()) {
          value.push_back(line[++i]);
        } else if (line[i] == '"') {
          return value;
        } else {
          value.push_back(line[i]);
        }
      }
      return std::nullopt;
    }

    std::optional<int> intAfter(std::string_view line, std::string_view key) {
      const auto at = line.find(key);
      if (at == std::string_view::npos) {
        return std::nullopt;
      }
      return std::atoi(std::string(line.substr(at + key.size())).c_str());
    }

    std::string formatNumber(double value) {
      std::string text = std::format("{:.6f}", value);
      while (text.ends_with('0')) {
        text.pop_back();
      }
      if (text.ends_with('.')) {
        text.pop_back();
      }
      return text;
    }

    std::optional<std::string> readFile(const fs::path& path) {
      std::ifstream in(path);
      if (!in) {
        return std::nullopt;
      }
      std::stringstream buffer;
      buffer << in.rdbuf();
      return buffer.str();
    }

    std::optional<std::string> validate(const fs::path& path) {
      process::RunOptions options;
      options.timeout = std::chrono::seconds(10);
      options.maxOutputBytes = 16384;
      const auto result = process::runSync({"niri", "validate", "-c", path.string()}, options);
      if (result) {
        return std::nullopt;
      }
      std::string message = trim(result.err).empty() ? result.out : result.err;
      // Lo útil de `niri validate` es el final (la causa); el principio son trazas de carga.
      if (message.size() > 600) {
        message = message.substr(message.size() - 600);
      }
      return message.empty() ? std::string("niri validate failed") : std::string(trim(message));
    }

    // Escribe al lado, valida con Niri y reemplaza de forma atómica: Niri nunca lee un archivo a medias ni
    // uno inválido.
    std::optional<std::string> writeValidated(const fs::path& path, const std::string& text) {
      std::error_code ec;
      fs::create_directories(path.parent_path(), ec);
      const fs::path tmp = path.string() + ".okaru-tmp";
      {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) {
          return std::format("cannot write {}", tmp.string());
        }
        out << text;
      }
      if (auto error = validate(tmp)) {
        fs::remove(tmp, ec);
        return error;
      }
      fs::rename(tmp, path, ec);
      if (ec) {
        fs::remove(tmp, ec);
        return std::format("cannot replace {}", path.string());
      }
      return std::nullopt;
    }

    std::string modeString(int width, int height, double refreshHz) {
      return std::format("{}x{}@{:.3f}Hz", width, height, refreshHz);
    }

  } // namespace

  fs::path configPath() {
    if (const char* custom = std::getenv("NIRI_CONFIG"); custom != nullptr && custom[0] != '\0') {
      return custom;
    }
    fs::path base;
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && xdg[0] != '\0') {
      base = xdg;
    } else if (const char* home = std::getenv("HOME"); home != nullptr) {
      base = fs::path(home) / ".config";
    }
    return base / "niri" / "config.kdl";
  }

  fs::path outputsPath() { return configPath().parent_path() / kOutputsInclude; }
  fs::path workspacesPath() { return configPath().parent_path() / kWorkspacesInclude; }

  bool hasOkaruIncludes(std::string_view configText) {
    bool outputs = false;
    bool workspaces = false;
    for (const auto line : lines(configText)) {
      if (!line.starts_with("include")) {
        continue; // también descarta las comentadas con //
      }
      outputs = outputs || line.find(kOutputsInclude) != std::string_view::npos;
      workspaces = workspaces || line.find(kWorkspacesInclude) != std::string_view::npos;
    }
    return outputs && workspaces;
  }

  std::string withOkaruIncludes(std::string_view configText) {
    std::string text(configText);
    if (!text.empty() && !text.ends_with('\n')) {
      text.push_back('\n');
    }
    text += "\n// OKARU: pantallas y workspaces que administra Ajustes.\n";
    text += std::format("include optional=true \"{}\"\n", kOutputsInclude);
    text += std::format("include optional=true \"{}\"\n", kWorkspacesInclude);
    return text;
  }

  bool includesActive() {
    const auto text = readFile(configPath());
    return text.has_value() && hasOkaruIncludes(*text);
  }

  std::optional<std::string> activateIncludes() {
    std::error_code ec;
    fs::path config = configPath();
    // config.kdl puede ser un enlace (dotfiles): se escribe en el destino para no reemplazar el enlace.
    if (fs::is_symlink(config, ec)) {
      config = fs::canonical(config, ec);
    }
    const auto text = readFile(config);
    if (!text.has_value()) {
      return std::format("cannot read {}", config.string());
    }
    if (hasOkaruIncludes(*text)) {
      return std::nullopt;
    }
    // Archivos vacíos para que Niri no avise en cada recarga de que faltan.
    for (const auto& [path, what] :
         {std::pair{outputsPath(), "Pantallas"}, std::pair{workspacesPath(), "Workspaces"}}) {
      if (!fs::exists(path, ec)) {
        fs::create_directories(path.parent_path(), ec);
        std::ofstream(path) << std::format("// OKARU: lo administra Ajustes → {}.\n", what);
      }
    }
    // Se valida el archivo final junto al original: los include relativos se resuelven igual.
    const fs::path tmp = config.parent_path() / "config.kdl.okaru-tmp";
    {
      std::ofstream out(tmp, std::ios::trunc);
      if (!out) {
        return std::format("cannot write {}", tmp.string());
      }
      out << withOkaruIncludes(*text);
    }
    if (auto error = validate(tmp)) {
      fs::remove(tmp, ec);
      return error;
    }
    fs::rename(tmp, config, ec);
    if (ec) {
      fs::remove(tmp, ec);
      return std::format("cannot replace {}", config.string());
    }
    return std::nullopt;
  }

  std::string kdlString(std::string_view value) {
    std::string out = "\"";
    for (const char c : value) {
      if (c == '"' || c == '\\') {
        out.push_back('\\');
      }
      out.push_back(c);
    }
    out.push_back('"');
    return out;
  }

  // ── Pantallas ─────────────────────────────────────────────────────────────────────────────────────────

  std::vector<OutputInfo> parseOutputsJson(const nlohmann::json& json) {
    std::vector<OutputInfo> infos;
    if (!json.is_object()) {
      return infos;
    }
    for (const auto& [name, output] : json.items()) {
      if (!output.is_object()) {
        continue;
      }
      OutputInfo info;
      info.name = name;
      const std::string make = output.value("make", std::string());
      const std::string model = output.value("model", std::string());
      info.description = std::string(trim(make + " " + model));
      std::vector<std::array<double, 3>> modes;
      if (const auto it = output.find("modes"); it != output.end() && it->is_array()) {
        for (const auto& mode : *it) {
          const int width = mode.value("width", 0);
          const int height = mode.value("height", 0);
          const double hz = mode.value("refresh_rate", 0) / 1000.0; // el IPC da milihercios
          modes.push_back({static_cast<double>(width), static_cast<double>(height), hz});
          info.modes.push_back(modeString(width, height, hz));
        }
      }
      if (const auto it = output.find("current_mode"); it != output.end() && it->is_number_unsigned()) {
        const auto index = it->get<std::size_t>();
        if (index < modes.size()) {
          info.width = static_cast<int>(modes[index][0]);
          info.height = static_cast<int>(modes[index][1]);
          info.refresh = modes[index][2];
        }
      }
      info.vrrCapable = output.value("vrr_supported", false);
      info.vrrActive = output.value("vrr_enabled", false);
      const auto logical = output.find("logical");
      if (logical == output.end() || !logical->is_object()) {
        info.disabled = true;
      } else {
        info.x = logical->value("x", 0);
        info.y = logical->value("y", 0);
        info.scale = logical->value("scale", 1.0);
        const std::string transform = logical->value("transform", std::string("Normal"));
        const auto it = std::ranges::find(kIpcTransforms, transform);
        info.transform = it == kIpcTransforms.end() ? 0 : static_cast<int>(it - kIpcTransforms.begin());
      }
      infos.push_back(std::move(info));
    }
    std::ranges::sort(infos, {}, &OutputInfo::name);
    return infos;
  }

  std::vector<OutputInfo> queryOutputs(const compositors::niri::NiriRuntime& runtime) {
    const auto response = runtime.requestJson("\"Outputs\"\n");
    if (!response.has_value() || !response->contains("Ok")) {
      return {};
    }
    auto infos = parseOutputsJson((*response)["Ok"].value("Outputs", nlohmann::json::object()));
    if (const auto focused = runtime.requestJson("\"FocusedOutput\"\n");
        focused.has_value() && focused->contains("Ok")) {
      const auto& value = (*focused)["Ok"].value("FocusedOutput", nlohmann::json());
      if (value.is_object()) {
        const std::string name = value.value("name", std::string());
        for (auto& info : infos) {
          info.focused = info.name == name;
        }
      }
    }
    return infos;
  }

  std::string serializeOutputs(const std::vector<OutputRule>& rules, std::string_view primary) {
    std::string out =
        "// OKARU: lo administra Ajustes → Pantallas. Los cambios a mano se pierden al guardar desde ahí.\n";
    for (const auto& rule : rules) {
      out += std::format("\noutput {} {{\n", kdlString(rule.output));
      if (rule.disabled) {
        out += "    off\n";
      } else {
        if (!rule.mode.empty()) {
          std::string mode = rule.mode;
          if (mode.ends_with("Hz")) {
            mode.resize(mode.size() - 2);
          }
          out += std::format("    mode {}\n", kdlString(mode));
        }
        out += std::format("    scale {}\n", formatNumber(rule.scale > 0.0 ? rule.scale : 1.0));
        out += std::format(
            "    transform {}\n",
            kdlString(kConfigTransforms[static_cast<std::size_t>(std::clamp(rule.transform, 0, 7))])
        );
        out += std::format("    position x={} y={}\n", rule.x, rule.y);
        if (rule.vrr == 1) {
          out += "    variable-refresh-rate\n";
        } else if (rule.vrr == 2) {
          out += "    variable-refresh-rate on-demand=true\n";
        }
      }
      if (!primary.empty() && rule.output == primary) {
        out += "    focus-at-startup\n";
      }
      out += "}\n";
    }
    return out;
  }

  // shortcut: solo entiende lo que escribe serializeOutputs (una propiedad por línea); si alguien edita el
  // archivo a mano con otra forma, se ignora esa línea. Pasar a un parser KDL si eso se vuelve común.
  std::vector<OutputRule> parseOutputs(std::string_view kdl, std::string* primary) {
    std::vector<OutputRule> rules;
    OutputRule* current = nullptr;
    for (const auto line : lines(kdl)) {
      if (line.starts_with("//") || line.empty()) {
        continue;
      }
      if (line.starts_with("output ")) {
        rules.push_back({.scale = 1.0, .vrr = 0});
        current = &rules.back();
        current->output = quoted(line).value_or("");
        continue;
      }
      if (current == nullptr) {
        continue;
      }
      if (line == "}") {
        current = nullptr;
      } else if (line == "off") {
        current->disabled = true;
      } else if (line.starts_with("mode ")) {
        current->mode = quoted(line).value_or("");
        if (!current->mode.empty() && current->mode.find('@') != std::string::npos) {
          current->mode += "Hz";
        }
      } else if (line.starts_with("scale ")) {
        current->scale = std::strtod(std::string(line.substr(6)).c_str(), nullptr);
      } else if (line.starts_with("transform ")) {
        const auto value = quoted(line).value_or("normal");
        const auto it = std::ranges::find(kConfigTransforms, value);
        current->transform = it == kConfigTransforms.end() ? 0 : static_cast<int>(it - kConfigTransforms.begin());
      } else if (line.starts_with("position ")) {
        current->x = intAfter(line, "x=").value_or(0);
        current->y = intAfter(line, "y=").value_or(0);
      } else if (line.starts_with("variable-refresh-rate")) {
        current->vrr = line.find("on-demand=true") != std::string_view::npos ? 2 : 1;
      } else if (line == "focus-at-startup" && primary != nullptr) {
        *primary = current->output;
      }
    }
    std::erase_if(rules, [](const OutputRule& r) { return r.output.empty(); });
    return rules;
  }

  OutputRule ruleFor(const OutputInfo& info, const std::vector<OutputRule>& fileRules) {
    const auto file = std::ranges::find(fileRules, info.name, &OutputRule::output);
    OutputRule rule{.output = info.name, .scale = 1.0, .vrr = info.vrrActive ? 1 : 0};
    if (info.disabled && file != fileRules.end()) {
      rule = *file; // apagado: el IPC no da modo ni posición; se conserva lo guardado
      rule.disabled = true;
      return rule;
    }
    if (info.width > 0) {
      rule.mode = modeString(info.width, info.height, info.refresh);
    } else if (!info.modes.empty()) {
      rule.mode = info.modes.front();
    }
    rule.x = info.x;
    rule.y = info.y;
    rule.scale = info.scale;
    rule.transform = info.transform;
    rule.disabled = info.disabled;
    if (file != fileRules.end() && (file->vrr == 2 || (file->vrr == 1) == info.vrrActive)) {
      rule.vrr = file->vrr;
    }
    return rule;
  }

  std::optional<std::string> writeOutputs(const std::vector<OutputRule>& rules, std::string_view primary) {
    return writeValidated(outputsPath(), serializeOutputs(rules, primary));
  }

  std::vector<OutputRule> readOutputs(std::string* primary) {
    const auto text = readFile(outputsPath());
    return text.has_value() ? parseOutputs(*text, primary) : std::vector<OutputRule>{};
  }

  // ── Workspaces ────────────────────────────────────────────────────────────────────────────────────────

  bool isValidWorkspaceName(std::string_view name) {
    if (trim(name).empty() || name.size() > 64) {
      return false;
    }
    return std::ranges::none_of(name, [](char c) { return static_cast<unsigned char>(c) < 0x20; });
  }

  std::string serializeWorkspaces(const std::vector<NamedWorkspace>& workspaces) {
    std::string out = "// OKARU: lo administra Ajustes → Workspaces. Niri los crea en este orden.\n";
    for (const auto& ws : workspaces) {
      if (ws.output.empty()) {
        out += std::format("\nworkspace {}\n", kdlString(ws.name));
      } else {
        out += std::format("\nworkspace {} {{\n    open-on-output {}\n}}\n", kdlString(ws.name), kdlString(ws.output));
      }
    }
    return out;
  }

  // shortcut: igual que parseOutputs, solo la forma que escribe serializeWorkspaces.
  std::vector<NamedWorkspace> parseWorkspaces(std::string_view kdl) {
    std::vector<NamedWorkspace> out;
    bool inBlock = false;
    for (const auto line : lines(kdl)) {
      if (line.starts_with("workspace ")) {
        out.push_back({.name = quoted(line).value_or("")});
        inBlock = line.ends_with('{');
      } else if (inBlock && line.starts_with("open-on-output ")) {
        out.back().output = quoted(line).value_or("");
      } else if (line == "}") {
        inBlock = false;
      }
    }
    std::erase_if(out, [](const NamedWorkspace& ws) { return ws.name.empty(); });
    return out;
  }

  std::vector<NamedWorkspace> readWorkspaces() {
    const auto text = readFile(workspacesPath());
    return text.has_value() ? parseWorkspaces(*text) : std::vector<NamedWorkspace>{};
  }

  std::optional<std::string> saveWorkspaces(
      const compositors::niri::NiriRuntime& runtime, const std::vector<NamedWorkspace>& before,
      const std::vector<NamedWorkspace>& after, std::string_view renamedFrom, std::string_view renamedTo
  ) {
    const auto byName = [](std::string_view name) { return nlohmann::json{{"Name", std::string(name)}}; };
    if (!renamedFrom.empty() && renamedFrom != renamedTo) {
      (void)runtime.requestAction(
          nlohmann::json{{"SetWorkspaceName", {{"name", std::string(renamedTo)}, {"workspace", byName(renamedFrom)}}}}
      );
    }
    if (auto error = writeValidated(workspacesPath(), serializeWorkspaces(after))) {
      return error;
    }
    // open-on-output solo decide dónde nace el workspace; uno que ya existe se mueve en vivo.
    for (const auto& ws : after) {
      const std::string_view previousName = ws.name == renamedTo ? renamedFrom : std::string_view(ws.name);
      const auto old = std::ranges::find(before, previousName, &NamedWorkspace::name);
      if (!ws.output.empty() && old != before.end() && old->output != ws.output) {
        (void)runtime.requestAction(
            nlohmann::json{{"MoveWorkspaceToMonitor", {{"output", ws.output}, {"reference", byName(ws.name)}}}}
        );
      }
    }
    return std::nullopt;
  }

} // namespace okaru::niri
