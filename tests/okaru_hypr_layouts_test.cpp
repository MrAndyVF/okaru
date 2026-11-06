// OKARU: bloque okaru:layouts de layout.lua y ciclo de layouts.
#include "compositors/hyprland/okaru_hypr_layouts.h"

#include <cstdlib>
#include <print>

using namespace okaru::hypr;

namespace {
  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "okaru_hypr_layouts_test: {}", message);
    }
    return condition;
  }

  constexpr const char* kFile = R"(hl.config({ general = { layout = "scrolling" } })

-- okaru:options:begin (lo escribe OKARU → Ventanas; va al final para ganar sobre lo anterior)
hl.config({ general = { gaps_out = 8 } }) -- base: 20
-- okaru:options:end
)";
} // namespace

int main() {
  bool ok = true;
  ok = expect(resolveLayout("scrolling", "next") == "dwindle", "next tras scrolling") && ok;
  ok = expect(resolveLayout("monocle", "next") == "scrolling", "next da la vuelta") && ok;
  ok = expect(resolveLayout("scrolling", "prev") == "monocle", "prev da la vuelta") && ok;
  ok = expect(resolveLayout("lua:grid", "next") == "scrolling", "desconocido entra por el primero") && ok;
  ok = expect(resolveLayout("dwindle", "master") == "master", "nombre directo") && ok;
  ok = expect(!resolveLayout("dwindle", "spiral").has_value(), "nombre inválido") && ok;

  const WorkspaceLayouts layouts{{"2", "dwindle"}, {"11", "master"}};
  const std::string withBlock = replaceLayoutsBlock(kFile, layouts);
  ok = expect(withBlock.find(R"(hl.workspace_rule({ workspace = "2", layout = "dwindle" }))") != std::string::npos,
              "regla del workspace 2")
      && ok;
  ok = expect(withBlock.find("-- okaru:options:end") < withBlock.find("-- okaru:layouts:begin"), "bloque al final")
      && ok;
  ok = expect(parseLayoutsBlock(withBlock) == layouts, "ida y vuelta") && ok;
  ok = expect(replaceLayoutsBlock(withBlock, {}) == kFile, "vacío quita el bloque sin dejar rastro") && ok;

  const WorkspaceLayouts one{{"3", "monocle"}};
  const std::string replaced = replaceLayoutsBlock(withBlock, one);
  ok = expect(parseLayoutsBlock(replaced) == one, "reemplazo en su lugar") && ok;
  ok = expect(replaced.find("workspace = \"2\"") == std::string::npos, "sin restos del bloque anterior") && ok;

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
