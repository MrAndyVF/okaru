#include "compositors/hyprland/okaru_hypr_animations.h"

#include "compositors/hyprland/hyprland_runtime.h"
#include "compositors/hyprland/okaru_hypr_options.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <regex>
#include <sstream>

namespace okaru::hypr {
  namespace {

    constexpr std::string_view kBegin = "-- okaru:animations:begin";
    constexpr std::string_view kEnd = "-- okaru:animations:end";

    AnimCurve bezier(std::string name, double x0, double y0, double x1, double y1) {
      return {.name = std::move(name), .spring = false, .x0 = x0, .y0 = y0, .x1 = x1, .y1 = y1};
    }

    // Resorte con masa 1 a partir de rigidez y ratio de amortiguación (1 = sin rebote).
    AnimCurve spring(std::string name, double stiffness, double ratio) {
      return {
          .name = std::move(name),
          .spring = true,
          .stiffness = stiffness,
          .damping = std::round(ratio * 2.0 * std::sqrt(stiffness) * 100.0) / 100.0,
          .mass = 1.0,
      };
    }

    std::string number(double value) {
      std::string text = std::format("{:.4f}", value);
      while (text.ends_with('0')) {
        text.pop_back();
      }
      if (text.ends_with('.')) {
        text.pop_back();
      }
      return text;
    }

    std::string readLayout() {
      std::ifstream in(okaFilePath("layout"), std::ios::binary);
      if (!in) {
        return {};
      }
      std::stringstream buffer;
      buffer << in.rdbuf();
      return buffer.str();
    }

  } // namespace

  const std::vector<AnimPreset>& animationPresets() {
    static const std::vector<AnimPreset> kPresets{
        // El estado del usuario al 2026-10-30: resortes críticamente amortiguados como niri.
        {"fluid",
         {spring("okaru_fluid", 800, 1.0), spring("okaru_fluid_ws", 1000, 1.0),
          bezier("okaru_fluid_ease", 0, 0.75, 0.15, 1)},
         {
             {"windowsIn", true, 3, "okaru_fluid_ease", ""},
             {"windowsOut", true, 6, "okaru_fluid_ease", ""},
             {"windowsMove", true, 4, "okaru_fluid", ""},
             {"fade", true, 3, "okaru_fluid_ease", ""},
             {"fadeOut", true, 6, "okaru_fluid_ease", ""},
             {"border", true, 3, "okaru_fluid_ease", ""},
             {"workspaces", true, 5, "okaru_fluid_ws", "slidevert"},
         }},
        // Material 3 Expressive (MotionScheme.expressive): rápido y con rebote.
        {"expressive",
         {spring("okaru_m3_fast", 800, 0.6), spring("okaru_m3_spatial", 700, 0.8),
          spring("okaru_m3_effects", 1600, 1.0), spring("okaru_m3_ws", 1000, 1.0),
          bezier("okaru_m3_decel", 0.05, 0.7, 0.1, 1), bezier("okaru_m3_accel", 0.3, 0, 0.8, 0.15)},
         {
             {"windowsIn", true, 3, "okaru_m3_fast", "popin 80%"},
             {"windowsOut", true, 3, "okaru_m3_effects", "popin 85%"},
             {"windowsMove", true, 4, "okaru_m3_spatial", ""},
             {"layersIn", true, 3, "okaru_m3_fast", "popin 90%"},
             {"fadeIn", true, 3, "okaru_m3_decel", ""},
             {"fade", true, 3, "okaru_m3_decel", ""},
             {"fadeOut", true, 2, "okaru_m3_accel", ""},
             {"border", true, 3, "okaru_m3_decel", ""},
             {"workspaces", true, 5, "okaru_m3_ws", "slidevert"},
         }},
        // Estilo macOS: suave, sin rebote apreciable.
        {"serene",
         {spring("okaru_serene", 500, 0.88), spring("okaru_serene_ws", 600, 0.95),
          bezier("okaru_serene_ease", 0.32, 0.72, 0, 1)},
         {
             {"windowsIn", true, 4, "okaru_serene", "popin 90%"},
             {"windowsOut", true, 4, "okaru_serene_ease", "popin 90%"},
             {"windowsMove", true, 5, "okaru_serene", ""},
             {"layers", true, 4, "okaru_serene_ease", "fade"},
             {"fade", true, 4, "okaru_serene_ease", ""},
             {"border", true, 4, "okaru_serene_ease", ""},
             {"workspaces", true, 5, "okaru_serene_ws", "slidevert"},
         }},
        // Corto y directo.
        {"snappy",
         {bezier("okaru_snap", 0.2, 0.9, 0.1, 1), bezier("okaru_snap_out", 0.4, 0, 1, 1)},
         {
             {"windowsIn", true, 2, "okaru_snap", "popin 90%"},
             {"windowsOut", true, 1.5, "okaru_snap_out", "popin 90%"},
             {"windowsMove", true, 2, "okaru_snap", ""},
             {"layers", true, 2, "okaru_snap", "fade"},
             {"fade", true, 2, "okaru_snap", ""},
             {"border", true, 2, "okaru_snap", ""},
             {"workspaces", true, 3, "okaru_snap", "slidevert"},
         }},
        // Rebote marcado.
        {"playful",
         {spring("okaru_bounce", 600, 0.5), spring("okaru_bounce_ws", 800, 0.65),
          bezier("okaru_bounce_ease", 0.05, 0.9, 0.1, 1)},
         {
             {"windowsIn", true, 3, "okaru_bounce", "popin 70%"},
             {"windowsOut", true, 3, "okaru_bounce_ease", "popin 80%"},
             {"windowsMove", true, 4, "okaru_bounce", ""},
             {"layersIn", true, 3, "okaru_bounce", "popin 85%"},
             {"fade", true, 3, "okaru_bounce_ease", ""},
             {"border", true, 3, "okaru_bounce_ease", ""},
             {"workspaces", true, 5, "okaru_bounce_ws", "slidevert"},
         }},
        // Reducir movimiento: nada se desplaza, solo desvanecidos cortos.
        {"minimal",
         {bezier("okaru_minimal", 0.25, 0.1, 0.25, 1)},
         {
             {"windows", true, 2, "okaru_minimal", "popin 100%"},
             {"windowsMove", false, 1, "okaru_minimal", ""},
             {"layers", true, 2, "okaru_minimal", "fade"},
             {"fade", true, 2, "okaru_minimal", ""},
             {"border", false, 1, "okaru_minimal", ""},
             {"workspaces", true, 2, "okaru_minimal", "fade"},
         }},
    };
    return kPresets;
  }

  const AnimPreset* findPreset(std::string_view id) {
    const auto& presets = animationPresets();
    const auto it = std::ranges::find(presets, id, &AnimPreset::id);
    return it != presets.end() ? &*it : nullptr;
  }

  bool isCustom(const AnimChoice& choice) { return choice.preset.empty() || choice.preset == "custom"; }

  std::vector<AnimCurve> okaruCurves() {
    std::vector<AnimCurve> curves;
    for (const auto& preset : animationPresets()) {
      for (const auto& curve : preset.curves) {
        if (!std::ranges::contains(curves, curve.name, &AnimCurve::name)) {
          curves.push_back(curve);
        }
      }
    }
    return curves;
  }

  AnimPreset resolvePreset(const AnimChoice& choice) {
    if (isCustom(choice)) {
      return {.id = "custom", .curves = okaruCurves(), .leaves = choice.custom};
    }
    const AnimPreset* base = findPreset(choice.preset);
    if (base == nullptr) {
      return {};
    }
    AnimPreset preset = *base;
    const double k = std::clamp(choice.duration, 0.25, 4.0);
    for (auto& curve : preset.curves) {
      if (curve.spring) {
        // Estirar el tiempo k veces con la misma forma: rigidez/k² y amortiguación/k.
        curve.stiffness = std::round(curve.stiffness / (k * k) * 100.0) / 100.0;
        curve.damping = std::round(curve.damping / k * 100.0) / 100.0;
      }
    }
    for (auto& leaf : preset.leaves) {
      leaf.speed = std::round(leaf.speed * k * 10.0) / 10.0;
      // "Mínimo" no desplaza escritorios: ahí manda el preset.
      if (leaf.leaf == "workspaces" && choice.preset != "minimal") {
        leaf.style = choice.workspaces;
      }
    }
    return preset;
  }

  std::string serializeAnimations(const AnimPreset& preset) {
    std::string out;
    for (const auto& c : preset.curves) {
      if (c.spring) {
        out += std::format(
            "hl.curve(\"{}\", {{ type = \"spring\", stiffness = {}, damping = {}, mass = {} }})\n", c.name,
            number(c.stiffness), number(c.damping), number(c.mass)
        );
      } else {
        out += std::format(
            "hl.curve(\"{}\", {{ type = \"bezier\", points = {{ {{ {}, {} }}, {{ {}, {} }} }} }})\n", c.name,
            number(c.x0), number(c.y0), number(c.x1), number(c.y1)
        );
      }
    }
    for (const auto& leaf : preset.leaves) {
      const auto curve = std::ranges::find(preset.curves, leaf.curve, &AnimCurve::name);
      // Las curvas del usuario no están en el preset: un resorte se marca con el prefijo "spring:".
      const bool spring = (curve != preset.curves.end() && curve->spring) || leaf.curve.starts_with("spring:");
      const std::string curveName = leaf.curve.starts_with("spring:") ? leaf.curve.substr(7) : leaf.curve;
      out += std::format(
          "hl.animation({{ leaf = \"{}\", enabled = {}, speed = {}, {} = \"{}\"", leaf.leaf,
          leaf.enabled ? "true" : "false", number(leaf.speed), spring ? "spring" : "bezier", curveName
      );
      if (!leaf.style.empty()) {
        out += std::format(", style = \"{}\"", leaf.style);
      }
      out += " })\n";
    }
    return out;
  }

  std::string serializeAnimationBlock(const AnimChoice& choice) {
    if (isCustom(choice) ? choice.custom.empty() : findPreset(choice.preset) == nullptr) {
      return {};
    }
    std::string out = std::format(
        "{} preset={} duration={} workspaces={} (lo escribe OKARU → Animaciones)\n", kBegin,
        isCustom(choice) ? std::string("custom") : choice.preset, number(choice.duration), choice.workspaces
    );
    out += serializeAnimations(resolvePreset(choice));
    out += kEnd;
    out += "\n";
    return out;
  }

  AnimChoice parseAnimationBlock(std::string_view lua) {
    static const std::regex kHeader(
        R"re(-- okaru:animations:begin preset=(\w+) duration=([0-9.]+) workspaces=(\w+))re"
    );
    AnimChoice choice;
    std::smatch m;
    const std::string text(lua);
    if (!std::regex_search(text, m, kHeader) || (m[1].str() != "custom" && findPreset(m[1].str()) == nullptr)) {
      return choice;
    }
    choice.preset = m[1].str();
    choice.duration = std::strtod(m[2].str().c_str(), nullptr);
    choice.workspaces = m[3].str();
    if (choice.preset != "custom") {
      return choice;
    }
    // Personalizado: las hl.animation del bloque son las ediciones del usuario.
    static const std::regex kLeaf(
        R"re(hl\.animation\(\{ leaf = "(\w+)", enabled = (true|false), speed = ([0-9.]+), (bezier|spring) = "([^"]+)"(?:, style = "([^"]*)")? \}\))re"
    );
    const auto begin = static_cast<std::size_t>(m.position(0));
    const auto end = text.find(kEnd, begin);
    const std::string block = text.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
    for (std::sregex_iterator it(block.begin(), block.end(), kLeaf), stop; it != stop; ++it) {
      const auto& f = *it;
      const bool spring = f[4].str() == "spring";
      const std::string curve = f[5].str();
      const bool isOkaru = std::ranges::contains(okaruCurves(), curve, &AnimCurve::name);
      choice.custom.push_back({
          .leaf = f[1].str(),
          .enabled = f[2].str() == "true",
          .speed = std::strtod(f[3].str().c_str(), nullptr),
          .curve = spring && !isOkaru ? "spring:" + curve : curve,
          .style = f[6].matched ? f[6].str() : std::string{},
      });
    }
    return choice;
  }

  std::string replaceAnimationBlock(std::string_view fileText, const AnimChoice& choice) {
    return replaceTrailingBlock(fileText, kBegin, kEnd, serializeAnimationBlock(choice));
  }

  AnimChoice readAnimationChoice() { return parseAnimationBlock(readLayout()); }

  std::optional<std::string> writeAnimationChoice(const AnimChoice& choice) {
    const std::string current = readLayout();
    const std::string next = replaceAnimationBlock(current, choice);
    if (next == current) {
      return std::nullopt;
    }
    const auto path = okaFilePath("layout");
    const auto tmp = path.string() + ".okaru-tmp";
    {
      std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
      if (!out || !(out << next) || !out.flush()) {
        return "could not write " + tmp;
      }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec); // atómico: Hyprland nunca lee un archivo a medias
    if (ec) {
      std::filesystem::remove(tmp, ec);
      return "could not replace " + path.string();
    }
    return std::nullopt;
  }

  std::optional<std::string>
  applyAnimationsLive(compositors::hyprland::HyprlandRuntime& runtime, const AnimChoice& choice) {
    if (isCustom(choice)) {
      // Personalizado: la config del usuario más sus ediciones, ya escritas en layout.lua. Recargar
      // también deshace las animaciones que se quitaron del bloque.
      (void)runtime.request("reload");
      return std::nullopt;
    }
    if (findPreset(choice.preset) == nullptr) {
      return std::nullopt;
    }
    const auto response = runtime.request("eval " + serializeAnimations(resolvePreset(choice)));
    if (!response.has_value()) {
      return "Hyprland did not respond";
    }
    if (response->empty() || response->starts_with("ok")) {
      return std::nullopt;
    }
    return *response;
  }

  double leafDurationMs(const AnimPreset& preset, const AnimLeaf& leaf) {
    const auto curve = std::ranges::find(preset.curves, leaf.curve, &AnimCurve::name);
    if (curve != preset.curves.end() && curve->spring) {
      // Hasta quedar a 0.5 % del destino.
      const double omega = std::sqrt(curve->stiffness / curve->mass);
      const double zeta = curve->damping / (2.0 * std::sqrt(curve->stiffness * curve->mass));
      const double rate = std::min(zeta, 1.0) * omega;
      return std::clamp(rate > 0.0 ? 5.3 / rate * 1000.0 : 1000.0, 100.0, 3000.0);
    }
    return std::max(50.0, leaf.speed * 100.0); // Hyprland: speed en décimas de segundo
  }

  namespace {
    // Herencia del árbol de animaciones de Hyprland 0.57.
    std::string_view parentOf(std::string_view leaf) {
      static const std::vector<std::pair<std::string_view, std::string_view>> kParents{
          {"windows", "global"},
          {"windowsIn", "windows"},
          {"windowsOut", "windows"},
          {"windowsMove", "windows"},
          {"layers", "global"},
          {"layersIn", "layers"},
          {"layersOut", "layers"},
          {"fade", "global"},
          {"fadeIn", "fade"},
          {"fadeOut", "fade"},
          {"fadeSwitch", "fade"},
          {"fadeShadow", "fade"},
          {"fadeDim", "fade"},
          {"fadeLayers", "fade"},
          {"fadeLayersIn", "fadeLayers"},
          {"fadeLayersOut", "fadeLayers"},
          {"fadePopups", "fade"},
          {"fadePopupsIn", "fadePopups"},
          {"fadePopupsOut", "fadePopups"},
          {"fadeDpms", "fade"},
          {"border", "global"},
          {"borderangle", "global"},
          {"workspaces", "global"},
          {"workspacesIn", "workspaces"},
          {"workspacesOut", "workspaces"},
          {"specialWorkspace", "workspaces"},
          {"specialWorkspaceIn", "specialWorkspace"},
          {"specialWorkspaceOut", "specialWorkspace"},
          {"zoomFactor", "global"},
          {"monitorAdded", "global"},
      };
      const auto it = std::ranges::find(kParents, leaf, &std::pair<std::string_view, std::string_view>::first);
      return it != kParents.end() ? it->second : std::string_view{};
    }
  } // namespace

  LiveAnimations parseAnimationsJson(const nlohmann::json& json) {
    LiveAnimations live;
    if (!json.is_array() || json.size() < 2 || !json[0].is_array()) {
      return live;
    }
    std::vector<LiveLeaf> raw;
    for (const auto& a : json[0]) {
      if (!a.is_object()) {
        continue;
      }
      std::string bezier = a.value("bezier", "");
      LiveLeaf leaf{
          .leaf = a.value("name", ""),
          .enabled = a.value("enabled", true),
          .speed = a.value("speed", 0.0),
          .curve = bezier.starts_with("spring:") ? bezier.substr(7) : bezier,
          .spring = bezier.starts_with("spring:"),
          .style = a.value("style", ""),
          .overridden = a.value("overridden", false),
      };
      if (leaf.spring && !std::ranges::contains(live.springs, leaf.curve)) {
        live.springs.push_back(leaf.curve);
      }
      raw.push_back(std::move(leaf));
    }
    if (json[1].is_array()) {
      for (const auto& c : json[1]) {
        if (c.is_object()) {
          live.beziers.push_back(c.value("name", ""));
        }
      }
    }
    // Resolver la herencia: una hoja no definida toma los valores de su padre.
    const auto find = [&raw](std::string_view name) -> const LiveLeaf* {
      const auto it = std::ranges::find(raw, name, &LiveLeaf::leaf);
      return it != raw.end() ? &*it : nullptr;
    };
    for (const auto& leaf : raw) {
      LiveLeaf effective = leaf;
      std::string_view parent = parentOf(leaf.leaf);
      const LiveLeaf* source = &leaf;
      while (!source->overridden && !parent.empty()) {
        if (const auto* p = find(parent)) {
          source = p;
        }
        parent = parentOf(parent);
      }
      effective.enabled = source->enabled;
      effective.speed = source->speed;
      effective.curve = source->curve;
      effective.spring = source->spring;
      if (effective.style.empty()) {
        effective.style = source->style;
      }
      live.leaves.push_back(std::move(effective));
    }
    return live;
  }

  LiveAnimations queryAnimations(compositors::hyprland::HyprlandRuntime& runtime) {
    const auto json = runtime.requestJson("j/animations");
    return json.has_value() ? parseAnimationsJson(*json) : LiveAnimations{};
  }

  std::function<float(float)> shellEasing(std::string_view style) {
    if (style == "minimal") {
      return {};
    }
    const AnimPreset preset = resolvePreset({.preset = std::string(style)});
    const auto leaf = std::ranges::find(preset.leaves, std::string("windowsMove"), &AnimLeaf::leaf);
    if (leaf == preset.leaves.end()) {
      return {};
    }
    const auto curve = std::ranges::find(preset.curves, leaf->curve, &AnimCurve::name);
    if (curve == preset.curves.end()) {
      return {};
    }
    const double duration = leafDurationMs(preset, *leaf);
    return [c = *curve, duration](float t) {
      return static_cast<float>(curveProgress(c, static_cast<double>(t) * duration, duration));
    };
  }

  double curveProgress(const AnimCurve& curve, double ms, double durationMs) {
    if (curve.spring) {
      const double t = ms / 1000.0;
      const double omega = std::sqrt(curve.stiffness / curve.mass);
      const double zeta = curve.damping / (2.0 * std::sqrt(curve.stiffness * curve.mass));
      if (zeta < 1.0) {
        const double wd = omega * std::sqrt(1.0 - zeta * zeta);
        return 1.0 - std::exp(-zeta * omega * t) * (std::cos(wd * t) + zeta * omega / wd * std::sin(wd * t));
      }
      return 1.0 - std::exp(-omega * t) * (1.0 + omega * t);
    }
    // Bezier cúbica (0,0) (x0,y0) (x1,y1) (1,1): buscar el parámetro con x = progreso de tiempo.
    const double x = std::clamp(ms / std::max(1.0, durationMs), 0.0, 1.0);
    const auto bez = [](double a, double b, double u) {
      return 3 * a * u * (1 - u) * (1 - u) + 3 * b * u * u * (1 - u) + u * u * u;
    };
    double lo = 0.0;
    double hi = 1.0;
    for (int i = 0; i < 40; ++i) {
      const double mid = (lo + hi) * 0.5;
      (bez(curve.x0, curve.x1, mid) < x ? lo : hi) = mid;
    }
    return bez(curve.y0, curve.y1, (lo + hi) * 0.5);
  }

} // namespace okaru::hypr
