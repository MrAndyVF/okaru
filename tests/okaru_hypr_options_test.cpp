// OKARU: opciones de Hyprland (descriptions, bloque de overrides al final del archivo).
#include "compositors/hyprland/okaru_hypr_options.h"

#include <cstdlib>
#include <print>
#include <string>

using namespace okaru::hypr;

namespace {

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "okaru_hypr_options_test: {}", message);
    }
    return condition;
  }

  constexpr const char* kLayout = R"(-- Distribución y decoración de ventanas.
hl.config({
	general = { gaps_in = 4, gaps_out = 8 },
})

hl.animation({ leaf = "fade", enabled = true, speed = 3, bezier = "default" })
)";

} // namespace

int main() {
  bool ok = true;

  // Tipos deducidos de `j/descriptions` (formato real de Hyprland 0.57).
  const auto descs = parseDescriptionsJson(nlohmann::json::parse(R"([
    {"name": "general:gaps_in", "description": "gaps", "default": "5 5 5 5", "current": "4 4 4 4", "min": null, "max": null},
    {"name": "decoration:blur:passes", "description": "passes", "default": 1, "current": 3, "min": 0, "max": 10, "map": null},
    {"name": "general:resize_on_border", "description": "x", "default": false, "current": false},
    {"name": "scrolling:column_width", "description": "x", "default": 0.5, "current": 0.5, "min": 0.1, "max": 1},
    {"name": "general:resize_corner", "description": "x", "default": 0, "current": 0, "min": 0, "max": 4,
     "map": [{"bottom_left": 4}, {"bottom_right": 3}, {"top_right": 2}, {"top_left": 1}, {"disabled": 0}]},
    {"name": "general:col.active_border", "description": "x", "default": "ffffffff 0deg", "current": "ffa2c9fe 0deg"},
    {"name": "general:layout", "description": "x", "default": "dwindle", "current": "scrolling"},
    {"name": "input:touchpad:tap-to-click", "description": "x", "default": true, "current": true}
  ])"));
  ok = expect(descs.size() == 8, "lee todas las descripciones") && ok;
  ok = expect(descs[7].name == "input:touchpad:tap_to_click", "guion -> guion bajo, como en la config Lua") && ok;
  ok = expect(descs[0].kind == OptionKind::Gaps && uniformGap(descs[0].current) == 4, "gaps uniformes") && ok;
  ok = expect(descs[1].kind == OptionKind::Int && descs[1].max == 10.0, "entero con rango") && ok;
  ok = expect(descs[2].kind == OptionKind::Bool, "booleano") && ok;
  ok = expect(descs[3].kind == OptionKind::Float && descs[3].min == 0.1, "decimal con rango") && ok;
  ok = expect(descs[4].kind == OptionKind::Enum && descs[4].choices.front().second == 0, "enum ordenado por valor")
      && ok;
  ok = expect(descs[5].kind == OptionKind::String, "un color no es gaps") && ok;
  ok = expect(descs[6].kind == OptionKind::String, "texto") && ok;
  ok = expect(!uniformGap(nlohmann::json("4 8 4 8")).has_value(), "gaps no uniformes") && ok;

  // Serialización anidada y claves que no son identificadores.
  ok = expect(
           serializeOption("decoration:blur:passes", 3) == "hl.config({ decoration = { blur = { passes = 3 } } })",
           "anidada"
       )
      && ok;
  ok = expect(
           serializeOption("general:col.active_border", "ff00ff00")
               == R"(hl.config({ general = { ["col.active_border"] = "ff00ff00" } }))",
           "clave con punto"
       )
      && ok;

  // Ida y vuelta del bloque.
  OptionValues values{
      {"general:gaps_out", 12},           {"decoration:blur:passes", 2}, {"decoration:active_opacity", 0.95},
      {"general:resize_on_border", true}, {"general:layout", "dwindle"}, {"general:col.inactive_border", "ff112233"},
  };
  const std::string written = replaceOptionsBlock(kLayout, values);
  ok = expect(parseOptionsBlock(written) == values, "ida y vuelta del bloque") && ok;
  ok = expect(written.starts_with(kLayout), "el bloque va al final y no toca el resto") && ok;
  ok = expect(replaceOptionsBlock(written, values) == written, "idempotente") && ok;

  // Cambiar y quitar.
  values["general:gaps_out"] = 6;
  values.erase("general:layout");
  const std::string changed = replaceOptionsBlock(written, values);
  ok = expect(parseOptionsBlock(changed) == values, "cambia el bloque") && ok;
  ok = expect(changed.starts_with(kLayout), "sigue sin tocar el resto") && ok;
  ok = expect(replaceOptionsBlock(changed, {}) == kLayout, "sin overrides el archivo vuelve a ser el original") && ok;

  // Valor original guardado junto al override, para saber cuándo se volvió a él.
  const OptionValues bases{{"general:gaps_out", "8 8 8 8"}, {"decoration:blur:passes", 3}};
  const OptionValues two{{"general:gaps_out", 12}, {"decoration:blur:passes", 2}};
  const std::string withBases = replaceOptionsBlock(kLayout, two, bases);
  OptionValues readBases;
  ok = expect(parseOptionsBlock(withBases, &readBases) == two, "valores con base") && ok;
  ok = expect(readBases == bases, "las bases se leen de vuelta") && ok;
  ok = expect(replaceOptionsBlock(withBases, two, bases) == withBases, "idempotente con bases") && ok;
  OptionValues noBases;
  ok = expect(parseOptionsBlock(changed, &noBases) == values && noBases.empty(), "bloque sin bases") && ok;

  // Comparación tal como reporta y escribe Hyprland.
  ok = expect(sameOptionValue("8 8 8 8", 8), "gaps CSS == número") && ok;
  ok = expect(!sameOptionValue("8 8 8 8", 12), "gaps distintos") && ok;
  ok = expect(sameOptionValue(0.9, 0.90000001), "decimales casi iguales") && ok;
  ok = expect(sameOptionValue(true, 1) && !sameOptionValue(false, 1), "booleanos") && ok;
  ok = expect(sameOptionValue("scrolling", "scrolling") && !sameOptionValue("scrolling", "dwindle"), "texto") && ok;

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
