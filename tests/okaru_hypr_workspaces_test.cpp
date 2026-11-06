// OKARU: bloque okaru:workspaces de outputs.lua.
#include "compositors/hyprland/okaru_hypr_workspaces.h"

#include <algorithm>
#include <cstdlib>
#include <print>

using namespace okaru::hypr;

namespace {
  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "okaru_hypr_workspaces_test: {}", message);
    }
    return condition;
  }

  constexpr const char* kFile = R"(-- okaru:monitors:begin
hl.monitor({ output = "DP-2", mode = "preferred", position = "0x0", scale = 1 })
-- okaru:monitors:end

hl.config({ misc = { vrr = 3 } })

return { workspace_names = names }
)";
} // namespace

int main() {
  bool ok = true;
  const std::vector<WorkspaceDef> defs{
      {.id = 2, .monitor = "DP-2", .name = "Games"},
      {.id = 1, .monitor = "DP-2", .name = "Per\"sonal", .isDefault = true},
      {.id = 11, .monitor = "DP-1", .name = "Personal", .isDefault = true},
  };
  const std::string block = serializeWorkspacesBlock(defs, "DP-2");
  ok = expect(
           block.find(R"(local names = { "Per\"sonal", "Games" })") != std::string::npos, "names del principal en orden"
       )
      && ok;

  const std::string first = replaceWorkspacesBlock(kFile, block);
  ok = expect(first.find(kWorkspacesBegin) < first.find("return { workspace_names"), "bloque antes del return") && ok;
  ok = expect(first.ends_with("return { workspace_names = names }\n"), "el return sigue al final") && ok;
  ok = expect(first.find("hl.config({ misc = { vrr = 3 } })") != std::string::npos, "conserva el resto") && ok;

  auto parsed = parseWorkspaces(first);
  ok = expect(parsed.size() == 3, "lee tres") && ok;
  auto sorted = defs;
  std::ranges::sort(sorted, [](const auto& a, const auto& b) {
    return a.monitor != b.monitor ? a.monitor < b.monitor : a.id < b.id;
  });
  ok = expect(parsed == sorted, "ida y vuelta (con comillas escapadas)") && ok;

  // Segunda escritura: reemplaza en su lugar y es idempotente.
  parsed[0].name = "Izq";
  const std::string second = replaceWorkspacesBlock(first, serializeWorkspacesBlock(parsed, "DP-2"));
  ok = expect(parseWorkspaces(second)[0].name == "Izq", "edita") && ok;
  ok = expect(replaceWorkspacesBlock(second, serializeWorkspacesBlock(parsed, "DP-2")) == second, "idempotente") && ok;

  ok = expect(nextWorkspaceId(defs, "DP-2") == 3 && nextWorkspaceId(defs, "DP-1") == 12, "siguiente id por monitor")
      && ok;
  ok = expect(nextWorkspaceId(defs, "HDMI-A-1") == 21, "monitor nuevo empieza una serie nueva") && ok;
  ok = expect(nextWorkspaceId({}, "Virtual-1") == 1, "instalación limpia empieza en 1") && ok;
  ok = expect(
           !isValidWorkspaceName("a\nb") && !isValidMonitorName("DP-1; x") && isValidMonitorName("HDMI-A-1"),
           "validación"
       )
      && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
