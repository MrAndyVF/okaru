// OKARU: estilos de animación para Hyprland (bloque okaru:animations, escala de duración, curvas).
#include "compositors/hyprland/okaru_hypr_animations.h"

#include <cmath>
#include <cstdlib>
#include <print>
#include <string>

using namespace okaru::hypr;

namespace {

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "okaru_hypr_animations_test: {}", message);
    }
    return condition;
  }

  constexpr const char* kLayout = R"(hl.config({ general = { gaps_in = 4 } })
hl.animation({ leaf = "fade", enabled = true, speed = 3, bezier = "default" })
)";

} // namespace

int main() {
  bool ok = true;

  ok = expect(animationPresets().size() == 6, "seis estilos") && ok;
  for (const auto& preset : animationPresets()) {
    for (const auto& leaf : preset.leaves) {
      bool found = false;
      for (const auto& curve : preset.curves) {
        found = found || curve.name == leaf.curve;
      }
      ok = expect(found, "cada animación usa una curva del mismo estilo") && ok;
    }
  }

  // Bloque: ida y vuelta, idempotente, y "Tu configuración" lo quita.
  const AnimChoice choice{.preset = "expressive", .duration = 1.5, .workspaces = "slide"};
  const std::string written = replaceAnimationBlock(kLayout, choice);
  ok = expect(parseAnimationBlock(written) == choice, "ida y vuelta del bloque") && ok;
  ok = expect(written.starts_with(kLayout), "va al final sin tocar el resto") && ok;
  ok = expect(replaceAnimationBlock(written, choice) == written, "idempotente") && ok;
  ok = expect(replaceAnimationBlock(written, AnimChoice{}) == kLayout, "sin estilo vuelve al original") && ok;
  ok = expect(parseAnimationBlock(kLayout).preset.empty(), "sin bloque no hay estilo") && ok;

  // Duración: bezier ×k, resorte rigidez/k² y amortiguación/k (misma forma, k veces más largo).
  const AnimPreset base = resolvePreset({.preset = "expressive", .duration = 1.0});
  const AnimPreset slow = resolvePreset({.preset = "expressive", .duration = 2.0});
  ok = expect(std::abs(slow.leaves[0].speed - base.leaves[0].speed * 2.0) < 0.05, "bezier: speed ×2") && ok;
  ok = expect(std::abs(slow.curves[0].stiffness - base.curves[0].stiffness / 4.0) < 0.05, "resorte: rigidez /4") && ok;
  ok =
      expect(std::abs(slow.curves[0].damping - base.curves[0].damping / 2.0) < 0.05, "resorte: amortiguación /2") && ok;
  const double baseMs = leafDurationMs(base, base.leaves[0]);
  const double slowMs = leafDurationMs(slow, slow.leaves[0]);
  ok = expect(std::abs(slowMs / baseMs - 2.0) < 0.1, "la vista previa dura el doble") && ok;

  // Estilo del cambio de escritorio; "Mínimo" siempre desvanece.
  const auto workspaces = [](const AnimPreset& p) {
    for (const auto& leaf : p.leaves) {
      if (leaf.leaf == "workspaces") {
        return leaf.style;
      }
    }
    return std::string{};
  };
  ok = expect(workspaces(resolvePreset({.preset = "fluid", .workspaces = "slide"})) == "slide", "escritorio horizontal")
      && ok;
  ok = expect(workspaces(resolvePreset({.preset = "minimal", .workspaces = "slide"})) == "fade", "mínimo desvanece")
      && ok;

  // Curvas de la vista previa: empiezan en 0 y terminan en 1; los resortes con rebote se pasan.
  for (const auto& preset : animationPresets()) {
    for (const auto& curve : preset.curves) {
      const double end = curveProgress(curve, 3000.0, 300.0);
      ok = expect(std::abs(curveProgress(curve, 0.0, 300.0)) < 1e-6, "la curva empieza en 0") && ok;
      ok = expect(std::abs(end - 1.0) < 0.01, "la curva termina en 1") && ok;
    }
  }
  const AnimCurve bouncy = resolvePreset({.preset = "playful"}).curves[0];
  double peak = 0.0;
  for (int ms = 0; ms < 1500; ms += 5) {
    peak = std::max(peak, curveProgress(bouncy, ms, 0.0));
  }
  ok = expect(peak > 1.05, "el estilo juguetón rebota") && ok;

  // Lo que se escribe tiene la forma que acepta Hyprland 0.57.
  const std::string lua = serializeAnimations(base);
  ok = expect(
           lua.find(R"(hl.curve("okaru_m3_fast", { type = "spring", stiffness = 800,)") != std::string::npos, "resorte"
       )
      && ok;
  ok = expect(lua.find(R"(type = "bezier", points = { { 0.05, 0.7 }, { 0.1, 1 } })") != std::string::npos, "bezier")
      && ok;
  ok = expect(
           lua.find(R"(leaf = "windowsIn", enabled = true, speed = 3, spring = "okaru_m3_fast", style = "popin 80%")")
               != std::string::npos,
           "animación con resorte y estilo"
       )
      && ok;

  // Personalizado: sin ediciones no hay bloque; con ediciones, ida y vuelta (curvas propias y del usuario).
  ok = expect(
           replaceAnimationBlock(kLayout, AnimChoice{.preset = "custom"}) == kLayout, "personalizado vacío sin bloque"
       )
      && ok;
  const AnimChoice custom{
      .preset = "custom",
      .custom = {
          {.leaf = "windowsIn", .enabled = true, .speed = 4, .curve = "okaru_m3_fast", .style = "popin 80%"},
          {.leaf = "workspaces", .enabled = true, .speed = 5, .curve = "spring:niri_ws", .style = "slidevert"},
          {.leaf = "fadeOut", .enabled = false, .speed = 2, .curve = "default", .style = ""},
      },
  };
  const std::string customText = replaceAnimationBlock(kLayout, custom);
  const AnimChoice customRead = parseAnimationBlock(customText);
  ok = expect(customRead.preset == "custom" && customRead.custom == custom.custom, "ida y vuelta personalizado") && ok;
  ok = expect(customText.find(R"(spring = "niri_ws", style = "slidevert")") != std::string::npos, "resorte del usuario")
      && ok;
  ok = expect(shellEasing("") == nullptr && shellEasing("minimal") == nullptr, "sin curva para la shell") && ok;
  const auto easing = shellEasing("playful");
  ok = expect(
           easing && std::abs(easing(0.0F)) < 1e-4F && std::abs(easing(1.0F) - 1.0F) < 0.02F, "curva de la shell 0..1"
       )
      && ok;

  // En vivo: herencia windowsIn <- windows <- global, y resortes con prefijo.
  const auto live = parseAnimationsJson(nlohmann::json::parse(R"([[
    {"name": "global", "overridden": true, "bezier": "default", "enabled": true, "speed": 8.0, "style": ""},
    {"name": "windows", "overridden": false, "bezier": "", "enabled": true, "speed": 0.0, "style": ""},
    {"name": "windowsIn", "overridden": false, "bezier": "", "enabled": true, "speed": 0.0, "style": ""},
    {"name": "windowsMove", "overridden": true, "bezier": "spring:niri", "enabled": true, "speed": 4.0, "style": ""}
  ], [{"name": "default", "X0": 0.0, "Y0": 0.75, "X1": 0.15, "Y1": 1.0}]])"));
  ok = expect(
           live.leaves.size() == 4 && live.leaves[2].speed == 8.0 && live.leaves[2].curve == "default",
           "hereda de global"
       )
      && ok;
  ok = expect(live.leaves[3].spring && live.leaves[3].curve == "niri" && live.springs.size() == 1, "resorte en vivo")
      && ok;
  ok = expect(live.beziers.size() == 1 && live.beziers[0] == "default", "curvas bezier") && ok;

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
