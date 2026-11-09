// OKARU: archivos de Pantallas y Workspaces para Niri (okaru/outputs.kdl y okaru/workspaces.kdl).
#include "compositors/niri/okaru_niri_config.h"

#include <cstdlib>
#include <print>

using namespace okaru::niri;

namespace {
  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "okaru_niri_config_test: {}", message);
    }
    return condition;
  }

  // Respuesta real de `Outputs` (niri 26.04 anidado), con un segundo monitor apagado.
  constexpr const char* kOutputsJson = R"({
    "winit": {"name": "winit", "make": "Smithay", "model": "Winit", "serial": null, "physical_size": null,
      "modes": [{"width": 940, "height": 1035, "refresh_rate": 60000, "is_preferred": true}],
      "current_mode": 0, "is_custom_mode": true, "vrr_supported": false, "vrr_enabled": false,
      "logical": {"x": 0, "y": 0, "width": 940, "height": 1035, "scale": 1.0, "transform": "Flipped180"}},
    "DP-1": {"name": "DP-1", "make": "LG", "model": "27GL850", "serial": "1",
      "modes": [{"width": 2560, "height": 1440, "refresh_rate": 143912, "is_preferred": true}],
      "current_mode": null, "vrr_supported": true, "vrr_enabled": false, "logical": null}
  })";
} // namespace

int main() {
  bool ok = true;

  // Pantallas: JSON del IPC.
  const auto infos = parseOutputsJson(nlohmann::json::parse(kOutputsJson));
  ok = expect(infos.size() == 2 && infos[0].name == "DP-1" && infos[1].name == "winit", "salidas ordenadas por nombre")
      && ok;
  ok = expect(
           infos[0].disabled && infos[0].vrrCapable && infos[0].modes.front() == "2560x1440@143.912Hz",
           "monitor apagado con sus modos en milihercios"
       )
      && ok;
  ok = expect(
           infos[1].transform == 6 && infos[1].width == 940 && infos[1].description == "Smithay Winit",
           "rotación Flipped180 y modo actual"
       )
      && ok;

  // Pantallas: ida y vuelta del archivo.
  const std::vector<OutputRule> rules{
      {.output = "DP-1", .mode = "2560x1440@143.912Hz", .x = 1920, .y = 0, .scale = 1.25, .transform = 1, .vrr = 2},
      {.output = "HDMI-A-1", .mode = "1920x1080@60.000Hz", .scale = 1.0, .vrr = 1, .disabled = true},
      {.output = "Some \"Co\" Monitor", .mode = "1920x1080@60.000Hz", .scale = 1.0, .vrr = 1},
  };
  const std::string kdl = serializeOutputs(rules, "DP-1");
  ok = expect(kdl.find("mode \"2560x1440@143.912\"") != std::string::npos, "modo sin el sufijo Hz") && ok;
  ok = expect(kdl.find("transform \"90\"") != std::string::npos, "rotación con nombre de Niri") && ok;
  ok = expect(kdl.find("variable-refresh-rate on-demand=true") != std::string::npos, "VRR bajo demanda") && ok;
  ok = expect(kdl.find("output \"Some \\\"Co\\\" Monitor\"") != std::string::npos, "comillas escapadas") && ok;
  std::string primary;
  const auto parsed = parseOutputs(kdl, &primary);
  ok = expect(primary == "DP-1", "focus-at-startup como monitor principal") && ok;
  ok = expect(parsed.size() == 3 && parsed[0] == rules[0], "DP-1 vuelve igual") && ok;
  ok = expect(parsed[1].disabled && parsed[1].output == "HDMI-A-1", "apagado vuelve apagado") && ok;
  ok = expect(parsed[2] == rules[2], "nombre con comillas vuelve igual") && ok;

  // La VRR bajo demanda solo vive en el archivo; el resto sale del estado en vivo.
  OutputInfo live{.name = "DP-1", .width = 2560, .height = 1440, .refresh = 143.912, .x = 5, .scale = 1.5};
  const auto rule = okaru::niri::ruleFor(live, parsed);
  ok = expect(
           rule.vrr == 2 && rule.x == 5 && rule.scale == 1.5 && rule.mode == "2560x1440@143.912Hz",
           "ruleFor mezcla vivo y archivo"
       )
      && ok;

  // Identidad: "marca modelo serie" solo con serie y si no se repite; el archivo la usa, la interfaz el conector.
  ok = expect(infos[0].identity == "LG 27GL850 1" && infos[1].identity.empty(), "identidad solo con serie") && ok;
  auto twins = parseOutputsJson(nlohmann::json::parse(R"({
    "DP-1": {"make": "LG", "model": "X", "serial": "7", "logical": null},
    "DP-2": {"make": "LG", "model": "X", "serial": "7", "logical": null}})"));
  ok = expect(twins[0].identity.empty() && twins[1].identity.empty(), "identidad repetida se descarta") && ok;
  const std::vector<OutputRule> lgOnly{{.output = "DP-1", .scale = 1.0, .vrr = 2}};
  const auto byIdentity = serializeOutputs(lgOnly, "DP-1", infos);
  ok = expect(
           byIdentity.find("output \"LG 27GL850 1\"") != std::string::npos
               && byIdentity.find("focus-at-startup") != std::string::npos,
           "se escribe por identidad, con su focus-at-startup"
       )
      && ok;
  std::string identityPrimary;
  const auto fromIdentity = parseOutputs(byIdentity, &identityPrimary);
  OutputInfo moved = infos[0];
  moved.name = "HDMI-A-1"; // mismo monitor en otro puerto
  moved.disabled = false;
  moved.width = 2560;
  const auto movedRule = okaru::niri::ruleFor(moved, fromIdentity);
  ok = expect(
           identityPrimary == "LG 27GL850 1" && movedRule.output == "HDMI-A-1" && movedRule.vrr == 2,
           "otro puerto recupera la regla y la interfaz ve el conector"
       )
      && ok;

  // Workspaces.
  const std::vector<NamedWorkspace> workspaces{{.name = "web", .output = "DP-1"}, {.name = "chat \"1\""}};
  const auto wsKdl = serializeWorkspaces(workspaces);
  ok = expect(
           wsKdl.find("workspace \"web\" {\n    open-on-output \"DP-1\"\n}") != std::string::npos, "bloque con monitor"
       )
      && ok;
  ok = expect(parseWorkspaces(wsKdl) == workspaces, "workspaces vuelven iguales y en orden") && ok;
  ok = expect(
           isValidWorkspaceName("Juegos") && !isValidWorkspaceName("  ") && !isValidWorkspaceName("a\nb"),
           "validación de nombres"
       )
      && ok;

  // Include en config.kdl.
  const std::string config = "input {\n}\n// include \"okaru/outputs.kdl\"\n";
  ok = expect(!hasOkaruIncludes(config), "un include comentado no cuenta") && ok;
  const auto activated = withOkaruIncludes(config);
  ok = expect(hasOkaruIncludes(activated), "las dos líneas quedan añadidas") && ok;
  ok = expect(activated.starts_with(config), "el resto de config.kdl no cambia") && ok;

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
