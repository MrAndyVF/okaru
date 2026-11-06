#include "compositors/hyprland/okaru_hypr_layouts.h"

#include "compositors/hyprland/hyprland_runtime.h"
#include "compositors/hyprland/okaru_hypr_options.h"

#include <algorithm>
#include <array>
#include <format>
#include <regex>
#include <sstream>

namespace okaru::hypr {
  namespace {

    constexpr std::string_view kBegin = "-- okaru:layouts:begin";
    constexpr std::string_view kEnd = "-- okaru:layouts:end";
    constexpr std::array<std::string_view, 4> kLayouts{"scrolling", "dwindle", "master", "monocle"};

    std::string ruleFor(std::string_view workspace, std::string_view layout) {
      return std::format(R"(hl.workspace_rule({{ workspace = "{}", layout = "{}" }}))", workspace, layout);
    }

    std::optional<std::string> evalLua(compositors::hyprland::HyprlandRuntime& runtime, const std::string& code) {
      const auto response = runtime.request("eval " + code);
      if (!response.has_value()) {
        return "Hyprland did not respond";
      }
      if (response->empty() || response->starts_with("ok")) {
        return std::nullopt;
      }
      return response;
    }

    std::string globalLayout(compositors::hyprland::HyprlandRuntime& runtime) {
      const auto json = runtime.requestJson("j/getoption general:layout");
      return json && json->contains("str") && (*json)["str"].is_string() ? (*json)["str"].get<std::string>()
                                                                          : std::string{};
    }

  } // namespace

  bool isKnownLayout(std::string_view layout) { return std::ranges::find(kLayouts, layout) != kLayouts.end(); }

  std::optional<std::string> resolveLayout(std::string_view current, std::string_view target) {
    if (target != "next" && target != "prev") {
      return isKnownLayout(target) ? std::optional<std::string>(target) : std::nullopt;
    }
    const auto it = std::ranges::find(kLayouts, current);
    if (it == kLayouts.end()) { // un layout de Lua u otro desconocido entra al ciclo por un extremo
      return std::string(target == "next" ? kLayouts.front() : kLayouts.back());
    }
    const auto index = static_cast<std::size_t>(it - kLayouts.begin());
    const std::size_t n = kLayouts.size();
    return std::string(kLayouts[target == "next" ? (index + 1) % n : (index + n - 1) % n]);
  }

  std::string serializeLayoutsBlock(const WorkspaceLayouts& layouts) {
    std::string out(kBegin);
    out += " (lo escribe OKARU al cambiar de layout por workspace)\n";
    for (const auto& [workspace, layout] : layouts) {
      out += ruleFor(workspace, layout) + "\n";
    }
    out += kEnd;
    out += '\n';
    return out;
  }

  WorkspaceLayouts parseLayoutsBlock(std::string_view lua) {
    WorkspaceLayouts layouts;
    const std::string text(lua);
    const auto begin = text.find(kBegin);
    if (begin == std::string::npos) {
      return layouts;
    }
    const auto end = text.find(kEnd, begin);
    std::istringstream in(text.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
    static const std::regex kLine(R"re(^hl\.workspace_rule\(\{ workspace = "([^"]+)", layout = "([^"]+)" \}\)$)re");
    std::string line;
    while (std::getline(in, line)) {
      std::smatch m;
      if (std::regex_match(line, m, kLine)) {
        layouts[m[1].str()] = m[2].str();
      }
    }
    return layouts;
  }

  std::string replaceLayoutsBlock(std::string_view fileText, const WorkspaceLayouts& layouts) {
    return replaceTrailingBlock(fileText, kBegin, kEnd, layouts.empty() ? std::string{} : serializeLayoutsBlock(layouts));
  }

  std::string activeLayout(compositors::hyprland::HyprlandRuntime& runtime) {
    const auto json = runtime.requestJson("j/activeworkspace");
    return json && json->contains("tiledLayout") && (*json)["tiledLayout"].is_string()
        ? (*json)["tiledLayout"].get<std::string>()
        : std::string{};
  }

  std::string outputLayout(compositors::hyprland::HyprlandRuntime& runtime, std::string_view output) {
    if (output.empty()) {
      return activeLayout(runtime);
    }
    const auto monitors = runtime.requestJson("j/monitors");
    const auto workspaces = runtime.requestJson("j/workspaces");
    if (!monitors || !workspaces || !monitors->is_array() || !workspaces->is_array()) {
      return {};
    }
    for (const auto& monitor : *monitors) {
      if (monitor.value("name", std::string{}) != output) {
        continue;
      }
      const auto id = monitor.contains("activeWorkspace") ? monitor["activeWorkspace"].value("id", 0LL) : 0LL;
      for (const auto& ws : *workspaces) {
        if (ws.value("id", 0LL) == id) {
          return ws.value("tiledLayout", std::string{});
        }
      }
    }
    return {};
  }

  std::string_view layoutGlyph(std::string_view layout) {
    if (layout == "scrolling") {
      return "carousel-horizontal";
    }
    if (layout == "dwindle") {
      return "layout-board-split";
    }
    if (layout == "master") {
      return "layout-sidebar";
    }
    if (layout == "monocle") {
      return "square";
    }
    return "layout-grid";
  }

  LayoutResult
  setLayout(compositors::hyprland::HyprlandRuntime& runtime, std::string_view target, bool perWorkspace) {
    const auto ws = runtime.requestJson("j/activeworkspace");
    if (!ws || !ws->contains("id") || !(*ws)["id"].is_number_integer()) {
      return {.error = "Hyprland did not respond"};
    }
    const auto id = (*ws)["id"].get<long long>();
    const std::string current = ws->value("tiledLayout", std::string{});
    const auto layout = resolveLayout(current, target);
    if (!layout) {
      return {.error = std::format("unknown layout: {}", target)};
    }

    const auto path = okaFilePath("layout");
    const std::string text = readOkaFile(path);
    auto layouts = parseLayoutsBlock(text);
    std::string next;

    if (perWorkspace) {
      if (id < 1) {
        return {.error = "special workspaces use the global layout"};
      }
      const std::string key = std::to_string(id);
      if (const auto err = evalLua(runtime, ruleFor(key, *layout))) {
        return {.error = *err};
      }
      // Si coincide con el general, el workspace ya lo sigue: la línea sobra.
      if (*layout == globalLayout(runtime)) {
        layouts.erase(key);
      } else {
        layouts[key] = *layout;
      }
      next = replaceLayoutsBlock(text, layouts);
    } else {
      const std::string oldGlobal = globalLayout(runtime);
      if (const auto err = applyOption(runtime, "general:layout", *layout)) {
        return {.error = *err};
      }
      // Hasta que Hyprland relea el archivo, las reglas de antes siguen vivas: se igualan ya.
      for (const auto& [workspace, _] : layouts) {
        (void)evalLua(runtime, ruleFor(workspace, *layout));
      }
      // Mismo criterio que Ajustes → Ventanas: el override guarda el valor de antes y se quita al volver a él.
      OptionValues bases;
      auto values = parseOptionsBlock(text, &bases);
      if (!values.contains("general:layout") && !oldGlobal.empty()) {
        bases["general:layout"] = oldGlobal;
      }
      if (const auto base = bases.find("general:layout"); base != bases.end() && base->second == *layout) {
        values.erase("general:layout");
        bases.erase("general:layout");
      } else {
        values["general:layout"] = *layout;
      }
      next = replaceLayoutsBlock(replaceOptionsBlock(text, values, bases), {});
    }

    if (next != text) {
      if (const auto err = writeFileAtomic(path, next)) {
        return {.layout = *layout, .error = *err};
      }
    }
    return {.layout = *layout};
  }

} // namespace okaru::hypr
