#include "compositors/hyprland/okaru_hypr_outputs.h"

#include "compositors/hyprland/hyprland_runtime.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <fstream>
#include <limits>
#include <regex>
#include <sstream>
#include <system_error>

namespace okaru::hypr {
  namespace {

    std::string trimNumber(double value) {
      // 1 -> "1", 1.25 -> "1.25", 1.666667 -> "1.666667"
      std::string text = std::format("{:.6f}", value);
      while (text.ends_with('0')) {
        text.pop_back();
      }
      if (text.ends_with('.')) {
        text.pop_back();
      }
      return text;
    }

    std::string luaString(std::string_view text) {
      std::string out = "\"";
      for (const char c : text) {
        if (c == '"' || c == '\\') {
          out += '\\';
        }
        out += c;
      }
      out += '"';
      return out;
    }

    bool parseModeSize(std::string_view mode, int& width, int& height) {
      const auto x = mode.find('x');
      if (x == std::string_view::npos) {
        return false;
      }
      const auto at = mode.find('@');
      const std::string w(mode.substr(0, x));
      const std::string h(mode.substr(x + 1, (at == std::string_view::npos ? mode.size() : at) - x - 1));
      char* end = nullptr;
      width = static_cast<int>(std::strtol(w.c_str(), &end, 10));
      if (end == w.c_str()) {
        return false;
      }
      height = static_cast<int>(std::strtol(h.c_str(), &end, 10));
      return end != h.c_str() && width > 0 && height > 0;
    }

    double parseModeRefresh(std::string_view mode) {
      const auto at = mode.find('@');
      if (at == std::string_view::npos) {
        return 0.0;
      }
      return std::strtod(std::string(mode.substr(at + 1)).c_str(), nullptr);
    }

  } // namespace

  std::vector<OutputInfo> parseMonitorsJson(const nlohmann::json& json) {
    std::vector<OutputInfo> out;
    if (!json.is_array()) {
      return out;
    }
    for (const auto& m : json) {
      if (!m.is_object()) {
        continue;
      }
      OutputInfo info;
      info.name = m.value("name", "");
      if (info.name.empty()) {
        continue;
      }
      info.description = m.value("description", "");
      info.width = m.value("width", 0);
      info.height = m.value("height", 0);
      info.refresh = m.value("refreshRate", 0.0);
      info.x = m.value("x", 0);
      info.y = m.value("y", 0);
      info.scale = m.value("scale", 1.0);
      info.transform = m.value("transform", 0);
      info.disabled = m.value("disabled", false);
      info.focused = m.value("focused", false);
      info.vrrActive = m.value("vrr", false);
      const std::string mirror = m.value("mirrorOf", "none");
      info.mirrorOf = mirror == "none" ? "" : mirror;
      info.cm = m.value("colorManagementPreset", "srgb");
      info.sdrBrightness = m.value("sdrBrightness", 1.0);
      info.sdrSaturation = m.value("sdrSaturation", 1.0);
      info.bitdepth = m.value("currentFormat", "").find("2101010") != std::string::npos ? 10 : 8;
      if (const auto it = m.find("hardwareDetails"); it != m.end() && it->is_object()) {
        info.vrrCapable = it->value("vrrCapable", false);
        info.hdrCapable = it->value("hdr", false);
      }
      if (const auto it = m.find("availableModes"); it != m.end() && it->is_array()) {
        for (const auto& mode : *it) {
          if (mode.is_string()) {
            info.modes.push_back(mode.get<std::string>());
          }
        }
      }
      out.push_back(std::move(info));
    }
    return out;
  }

  std::vector<OutputInfo> queryOutputs(compositors::hyprland::HyprlandRuntime& runtime) {
    const auto json = runtime.requestJson("j/monitors all");
    return json.has_value() ? parseMonitorsJson(*json) : std::vector<OutputInfo>{};
  }

  std::vector<OutputRule> parseRules(std::string_view lua) {
    static const std::regex kRule(R"re(hl\.monitor\s*\(\s*\{((?:[^{}]|\{[^{}]*\})*)\}\s*\))re");
    static const std::regex kReserved(R"re(\breserved\s*=\s*(?:(-?[0-9]+)|\{([^}]*)\}))re");
    static const std::regex kSide(R"re(\b(top|right|bottom|left)\s*=\s*(-?[0-9]+))re");
    static const std::regex kField(R"re((\w+)\s*=\s*(?:"((?:[^"\\]|\\.)*)"|(true|false)|(-?[0-9.]+)))re");

    std::vector<OutputRule> rules;
    const std::string text(lua);
    for (std::sregex_iterator it(text.begin(), text.end(), kRule), end; it != end; ++it) {
      const std::string body = (*it)[1].str();
      OutputRule rule;
      bool hasOutput = false;
      for (std::sregex_iterator f(body.begin(), body.end(), kField); f != end; ++f) {
        const std::string key = (*f)[1].str();
        const bool isString = (*f)[2].matched;
        const std::string str = (*f)[2].str();
        const bool boolean = (*f)[3].str() == "true";
        const double number = (*f)[4].matched ? std::strtod((*f)[4].str().c_str(), nullptr) : 0.0;
        if (key == "output") {
          rule.output = str;
          hasOutput = true;
        } else if (key == "mode") {
          rule.mode = str;
        } else if (key == "position") {
          int px = 0;
          int py = 0;
          if (std::sscanf(str.c_str(), "%dx%d", &px, &py) == 2) {
            rule.x = px;
            rule.y = py;
          }
        } else if (key == "scale") {
          rule.scale = isString ? std::strtod(str.c_str(), nullptr) : number;
        } else if (key == "transform") {
          rule.transform = static_cast<int>(number);
        } else if (key == "vrr") {
          rule.vrr = static_cast<int>(number);
        } else if (key == "bitdepth") {
          rule.bitdepth = static_cast<int>(number);
        } else if (key == "cm") {
          rule.cm = str;
        } else if (key == "sdrbrightness") {
          rule.sdrBrightness = number;
        } else if (key == "sdrsaturation") {
          rule.sdrSaturation = number;
        } else if (key == "mirror") {
          rule.mirror = str;
        } else if (key == "disabled") {
          rule.disabled = boolean;
        }
      }
      std::smatch reserved;
      if (std::regex_search(body, reserved, kReserved)) {
        if (reserved[1].matched) {
          const int all = std::atoi(reserved[1].str().c_str());
          rule.reservedTop = rule.reservedRight = rule.reservedBottom = rule.reservedLeft = all;
        } else {
          const std::string table = reserved[2].str();
          for (std::sregex_iterator s(table.begin(), table.end(), kSide), end; s != end; ++s) {
            const std::string side = (*s)[1].str();
            const int value = std::atoi((*s)[2].str().c_str());
            (side == "top"          ? rule.reservedTop
                 : side == "right"  ? rule.reservedRight
                 : side == "bottom" ? rule.reservedBottom
                                    : rule.reservedLeft) = value;
          }
        }
      }
      if (hasOutput && !rule.output.empty()) {
        std::erase_if(rules, [&](const OutputRule& r) { return r.output == rule.output; }); // la última gana
        rules.push_back(std::move(rule));
      }
    }
    return rules;
  }

  std::string closestMode(const OutputInfo& info) {
    std::string best;
    double bestDiff = std::numeric_limits<double>::max();
    for (const auto& mode : info.modes) {
      int w = 0;
      int h = 0;
      if (!parseModeSize(mode, w, h) || w != info.width || h != info.height) {
        continue;
      }
      const double diff = std::abs(parseModeRefresh(mode) - info.refresh);
      if (diff < bestDiff) {
        bestDiff = diff;
        best = mode;
      }
    }
    if (best.empty()) {
      best = std::format("{}x{}@{:.2f}Hz", info.width, info.height, info.refresh);
    }
    return best;
  }

  OutputRule ruleFor(const OutputInfo& info, const std::vector<OutputRule>& fileRules) {
    OutputRule rule;
    rule.output = info.name;
    rule.mode = closestMode(info);
    rule.x = info.x;
    rule.y = info.y;
    rule.scale = info.scale;
    rule.transform = info.transform;
    rule.bitdepth = info.bitdepth;
    rule.cm = info.cm;
    rule.sdrBrightness = info.sdrBrightness;
    rule.sdrSaturation = info.sdrSaturation;
    rule.mirror = info.mirrorOf;
    rule.disabled = info.disabled;
    if (const auto it = std::ranges::find(fileRules, info.name, &OutputRule::output); it != fileRules.end()) {
      rule.vrr = it->vrr;
      rule.bitdepth = it->bitdepth;
      rule.cm = it->cm;
      rule.reservedTop = it->reservedTop;
      rule.reservedRight = it->reservedRight;
      rule.reservedBottom = it->reservedBottom;
      rule.reservedLeft = it->reservedLeft;
      if (info.disabled) {
        // Un monitor apagado no reporta modo ni posición útiles: se conservan los del archivo.
        rule.mode = it->mode.empty() ? rule.mode : it->mode;
        rule.x = it->x;
        rule.y = it->y;
        rule.scale = it->scale;
        rule.transform = it->transform;
      }
    }
    return rule;
  }

  std::string serializeRule(const OutputRule& rule, bool explicitDefaults) {
    std::string out = "hl.monitor({ output = " + luaString(rule.output);
    out += ", mode = " + luaString(rule.mode.empty() ? "preferred" : rule.mode);
    out += std::format(", position = \"{}x{}\"", rule.x, rule.y);
    out += ", scale = " + trimNumber(rule.scale);
    if (explicitDefaults || rule.transform != 0) {
      out += std::format(", transform = {}", rule.transform);
    }
    if (rule.vrr >= 0) {
      out += std::format(", vrr = {}", rule.vrr);
    }
    out += std::format(", bitdepth = {}", rule.bitdepth);
    out += ", cm = " + luaString(rule.cm.empty() ? "srgb" : rule.cm);
    if (explicitDefaults || rule.sdrBrightness != 1.0) {
      out += ", sdrbrightness = " + trimNumber(rule.sdrBrightness);
    }
    if (explicitDefaults || rule.sdrSaturation != 1.0) {
      out += ", sdrsaturation = " + trimNumber(rule.sdrSaturation);
    }
    if (explicitDefaults || !rule.mirror.empty()) {
      out += ", mirror = " + luaString(rule.mirror);
    }
    if (explicitDefaults
        || rule.reservedTop != 0
        || rule.reservedRight != 0
        || rule.reservedBottom != 0
        || rule.reservedLeft != 0) {
      out += std::format(
          ", reserved = {{ top = {}, right = {}, bottom = {}, left = {} }}", rule.reservedTop, rule.reservedRight,
          rule.reservedBottom, rule.reservedLeft
      );
    }
    if (explicitDefaults || rule.disabled) {
      out += std::format(", disabled = {}", rule.disabled ? "true" : "false");
    }
    out += " })";
    return out;
  }

  std::string serializeBlock(const std::vector<OutputRule>& rules, std::string_view primary) {
    std::string out(kBlockBegin);
    out += " (lo escribe OKARU → Pantallas; los cambios a mano dentro del bloque se pierden)\n";
    for (const auto& rule : rules) {
      out += serializeRule(rule) + "\n";
    }
    out += R"(hl.monitor({ output = "", mode = "preferred", position = "auto", scale = "auto" }))";
    out += "\n";
    if (!primary.empty()) {
      out += "hl.config({ cursor = { default_monitor = " + luaString(primary) + " } })\n";
    }
    out += kBlockEnd;
    out += "\n";
    return out;
  }

  namespace {

    std::string replaceBlockOnly(std::string_view fileText, std::string_view block) {
      const std::string text(fileText);
      const auto begin = text.find(kBlockBegin);
      if (begin != std::string::npos) {
        const auto endMarker = text.find(kBlockEnd, begin);
        if (endMarker != std::string::npos) {
          auto end = endMarker + kBlockEnd.size();
          if (end < text.size() && text[end] == '\n') {
            ++end;
          }
          const auto lineStart = text.rfind('\n', begin);
          const auto start = lineStart == std::string::npos ? 0 : lineStart + 1;
          return text.substr(0, start) + std::string(block) + text.substr(end);
        }
      }

      // Primera vez: el bloque ocupa el lugar de las líneas hl.monitor sueltas.
      std::istringstream in(text);
      std::string line;
      std::string out;
      bool inserted = false;
      while (std::getline(in, line)) {
        const auto first = line.find_first_not_of(" \t");
        if (first != std::string::npos && std::string_view(line).substr(first).starts_with("hl.monitor(")) {
          if (!inserted) {
            out += block;
            inserted = true;
          }
          continue;
        }
        out += line + "\n";
      }
      if (!inserted) {
        out = std::string(block) + out;
      }
      if (!text.ends_with('\n') && out.ends_with('\n') && !text.empty()) {
        out.pop_back();
      }
      return out;
    }

    // Línea suelta que fija el monitor principal: `cursor = { default_monitor = "X" },` o
    // `default_monitor = "X",` dentro de una tabla cursor de varias líneas.
    bool isLoosePrimaryLine(const std::string& line) {
      static const std::regex kLoose(
          R"re(^\s*(cursor\s*=\s*\{\s*default_monitor\s*=\s*"[^"]*"\s*\}|default_monitor\s*=\s*"[^"]*")\s*,?\s*$)re"
      );
      return std::regex_match(line, kLoose);
    }

  } // namespace

  std::string replaceManagedBlock(std::string_view fileText, std::string_view block) {
    std::string out = replaceBlockOnly(fileText, block);
    if (block.find("default_monitor") == std::string_view::npos) {
      return out;
    }
    std::istringstream in(out);
    std::string line;
    std::string kept;
    bool inBlock = false;
    while (std::getline(in, line)) {
      if (line.find(kBlockBegin) != std::string::npos) {
        inBlock = true;
      }
      if (inBlock || !isLoosePrimaryLine(line)) {
        kept += line + "\n";
      }
      if (line.find(kBlockEnd) != std::string::npos) {
        inBlock = false;
      }
    }
    if (!out.ends_with('\n') && kept.ends_with('\n')) {
      kept.pop_back();
    }
    return kept;
  }

  std::filesystem::path outputsFilePath() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    const std::filesystem::path base = xdg != nullptr && xdg[0] != '\0'
        ? std::filesystem::path(xdg)
        : std::filesystem::path(home != nullptr ? home : "") / ".config";
    return base / "hypr" / "oka" / "outputs.lua";
  }

  namespace {
    std::string readOutputsFile() {
      std::ifstream in(outputsFilePath(), std::ios::binary);
      if (!in) {
        return {};
      }
      std::stringstream buffer;
      buffer << in.rdbuf();
      return buffer.str();
    }
  } // namespace

  std::vector<OutputRule> readFileRules() { return parseRules(readOutputsFile()); }

  std::string readFilePrimary() { return parsePrimary(readOutputsFile()); }

  std::string parsePrimary(std::string_view lua) {
    static const std::regex kPrimary(R"re(default_monitor\s*=\s*"([^"]*)")re");
    std::string primary;
    const std::string text(lua);
    for (std::sregex_iterator it(text.begin(), text.end(), kPrimary), end; it != end; ++it) {
      primary = (*it)[1].str(); // la última gana, como en Hyprland
    }
    return primary;
  }

  std::string queryPrimary(compositors::hyprland::HyprlandRuntime& runtime) {
    const auto json = runtime.requestJson("j/getoption cursor:default_monitor");
    if (!json.has_value() || !json->is_object()) {
      return {};
    }
    const std::string value = json->value("str", "");
    return value == "[[EMPTY]]" ? std::string{} : value;
  }

  std::optional<std::string> applyLive(
      compositors::hyprland::HyprlandRuntime& runtime, const std::vector<OutputRule>& rules, std::string_view primary
  ) {
    std::string code;
    for (const auto& rule : rules) {
      code += serializeRule(rule, /*explicitDefaults=*/true) + "\n";
    }
    code += "hl.config({ cursor = { default_monitor = " + luaString(primary) + " } })\n";
    const auto response = runtime.request("eval " + code);
    if (!response.has_value()) {
      return "Hyprland did not respond";
    }
    if (response->starts_with("ok") || response->empty()) {
      return std::nullopt;
    }
    return *response;
  }

  std::optional<std::string> writeFileRules(const std::vector<OutputRule>& rules, std::string_view primary) {
    const auto path = outputsFilePath();
    std::string current;
    {
      std::ifstream in(path, std::ios::binary);
      if (in) {
        std::stringstream buffer;
        buffer << in.rdbuf();
        current = buffer.str();
      }
    }
    const std::string next = replaceManagedBlock(current, serializeBlock(rules, primary));
    if (next == current) {
      return std::nullopt;
    }

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const auto tmp = path.string() + ".okaru-tmp";
    {
      std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
      if (!out) {
        return "could not write " + tmp;
      }
      out << next;
      if (!out.flush()) {
        return "could not write " + tmp;
      }
    }
    std::filesystem::rename(tmp, path, ec); // atómico: Hyprland nunca lee un archivo a medias
    if (ec) {
      std::filesystem::remove(tmp, ec);
      return "could not replace " + path.string();
    }
    return std::nullopt;
  }

  LogicalSize logicalSize(const OutputRule& rule) {
    int w = 0;
    int h = 0;
    if (!parseModeSize(rule.mode, w, h)) {
      w = 1920;
      h = 1080;
    }
    const double scale = rule.scale > 0.0 ? rule.scale : 1.0;
    LogicalSize size{.width = w / scale, .height = h / scale};
    if (rule.transform % 2 == 1) {
      std::swap(size.width, size.height);
    }
    return size;
  }

  Rect snapRect(Rect moving, const std::vector<Rect>& others, double threshold) {
    double bestDx = threshold + 1.0;
    double bestDy = threshold + 1.0;
    double dx = 0.0;
    double dy = 0.0;
    const auto consider = [](double delta, double& best, double& chosen) {
      if (std::abs(delta) < std::abs(best)) {
        best = delta;
        chosen = delta;
      }
    };
    for (const auto& o : others) {
      // Bordes verticales: pegar a izquierda/derecha y alinear lados.
      consider(o.x + o.width - moving.x, bestDx, dx);
      consider(o.x - (moving.x + moving.width), bestDx, dx);
      consider(o.x - moving.x, bestDx, dx);
      consider(o.x + o.width - (moving.x + moving.width), bestDx, dx);
      // Bordes horizontales: pegar arriba/abajo y alinear lados.
      consider(o.y + o.height - moving.y, bestDy, dy);
      consider(o.y - (moving.y + moving.height), bestDy, dy);
      consider(o.y - moving.y, bestDy, dy);
      consider(o.y + o.height - (moving.y + moving.height), bestDy, dy);
    }
    if (std::abs(bestDx) <= threshold) {
      moving.x += dx;
    }
    if (std::abs(bestDy) <= threshold) {
      moving.y += dy;
    }
    return moving;
  }

  void normalizePositions(std::vector<OutputRule>& rules) {
    int minX = std::numeric_limits<int>::max();
    int minY = std::numeric_limits<int>::max();
    for (const auto& rule : rules) {
      if (rule.disabled || !rule.mirror.empty()) {
        continue;
      }
      minX = std::min(minX, rule.x);
      minY = std::min(minY, rule.y);
    }
    if (minX == std::numeric_limits<int>::max()) {
      return;
    }
    for (auto& rule : rules) {
      rule.x -= minX;
      rule.y -= minY;
    }
  }

} // namespace okaru::hypr
