// OKARU: lectura, serialización y bloque administrado de monitores para Hyprland.
#include "compositors/hyprland/okaru_hypr_outputs.h"

#include <cstdlib>
#include <print>
#include <string>

using namespace okaru::hypr;

namespace {

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "okaru_hypr_outputs_test: {}", message);
    }
    return condition;
  }

  // Igual que ~/.config/hypr/oka/outputs.lua el 2026-11-01.
  constexpr const char* kUserFile = R"(-- Monitores y workspaces por monitor. Administrado por OKARU (Pantallas).
hl.monitor({ output = "DP-1", mode = "1920x1080@144.00Hz", position = "0x0", scale = 1, bitdepth = 8, cm = "srgb" })
hl.monitor({ output = "DP-2", mode = "1920x1080@144.00Hz", position = "1920x0", scale = 1, bitdepth = 8, cm = "srgb" })
hl.monitor({ output = "", mode = "preferred", position = "auto", scale = "auto" })

hl.config({
	misc = { vrr = 3 },
	-- Foco inicial en DP-2 (principal)
	cursor = { default_monitor = "DP-2" },
})

return { workspace_names = names }
)";

} // namespace

int main() {
  bool ok = true;

  const auto rules = parseRules(kUserFile);
  ok = expect(rules.size() == 2, "debe leer 2 reglas e ignorar la comodín") && ok;
  ok = expect(rules[1].output == "DP-2" && rules[1].x == 1920 && rules[1].y == 0, "posición de DP-2") && ok;
  ok = expect(rules[0].mode == "1920x1080@144.00Hz" && rules[0].scale == 1.0, "modo y escala") && ok;
  ok =
      expect(rules[0].vrr == -1 && rules[0].bitdepth == 8 && rules[0].cm == "srgb", "vrr heredado, 8 bits, srgb") && ok;

  // Ida y vuelta: lo que se escribe se vuelve a leer igual.
  OutputRule custom{
      .output = "HDMI-A-1",
      .mode = "2560x1440@165.00Hz",
      .x = -2560,
      .y = 120,
      .scale = 1.25,
      .transform = 1,
      .vrr = 2,
      .bitdepth = 10,
      .cm = "hdr",
      .sdrBrightness = 1.2,
      .sdrSaturation = 0.95,
      .mirror = "",
      .disabled = false,
      .reservedTop = 40,
      .reservedLeft = 12,
  };
  ok = expect(parseRules(serializeRule(custom)).front() == custom, "ida y vuelta de una regla completa") && ok;
  ok = expect(parseRules(serializeRule(custom, true)).front() == custom, "ida y vuelta con valores explícitos") && ok;
  ok =
      expect(serializeRule(rules[0]).find("transform") == std::string::npos, "sin campos por defecto en archivo") && ok;
  ok = expect(serializeRule(rules[0], true).find("mirror = \"\"") != std::string::npos, "en vivo limpia el espejo")
      && ok;

  // Área reservada: tabla por lado o número para los cuatro; la tabla no corta la regla.
  const auto reserved = parseRules(
      "hl.monitor({ output = \"A\", reserved = { top = 30, bottom = 5 }, scale = 2 })\n"
      "hl.monitor({ output = \"B\", reserved = 40 })\n"
  );
  ok = expect(
           reserved.size() == 2
               && reserved[0].reservedTop == 30
               && reserved[0].reservedBottom == 5
               && reserved[0].reservedRight == 0
               && reserved[0].scale == 2.0,
           "reserved en tabla"
       )
      && ok;
  ok = expect(reserved[1].reservedLeft == 40 && reserved[1].reservedTop == 40, "reserved como número") && ok;
  ok = expect(serializeRule(rules[0]).find("reserved") == std::string::npos, "sin reserved si es 0") && ok;

  // Primera escritura: el bloque reemplaza las hl.monitor sueltas y conserva el resto del archivo.
  const std::string first = replaceManagedBlock(kUserFile, serializeBlock(rules));
  ok = expect(first.find(kBlockBegin) != std::string::npos && first.find(kBlockEnd) != std::string::npos, "marcadores")
      && ok;
  ok = expect(first.find("misc = { vrr = 3 }") != std::string::npos, "conserva hl.config") && ok;
  ok = expect(first.find("return { workspace_names = names }") != std::string::npos, "conserva el return") && ok;
  ok = expect(first.starts_with("-- Monitores y workspaces"), "conserva el encabezado") && ok;
  ok = expect(parseRules(first) == rules, "las reglas no cambian tras la primera escritura") && ok;

  // Escrituras siguientes: solo cambia el bloque y es idempotente.
  auto moved = rules;
  moved[1].x = 0;
  moved[1].y = -1080;
  const std::string second = replaceManagedBlock(first, serializeBlock(moved));
  ok = expect(parseRules(second) == moved, "segunda escritura") && ok;
  ok = expect(replaceManagedBlock(second, serializeBlock(moved)) == second, "idempotente") && ok;
  ok = expect(second.find("misc = { vrr = 3 }") != std::string::npos, "segunda escritura conserva el resto") && ok;

  // Archivo sin reglas: el bloque va al principio.
  const std::string empty = replaceManagedBlock("return {}\n", serializeBlock(rules));
  ok = expect(empty.starts_with(kBlockBegin) && empty.ends_with("return {}\n"), "archivo sin reglas") && ok;

  // Monitor principal: se lee del archivo, va dentro del bloque y quita la línea suelta que lo pisaría.
  ok = expect(parsePrimary(kUserFile) == "DP-2", "lee el monitor principal") && ok;
  const std::string withPrimary = replaceManagedBlock(kUserFile, serializeBlock(rules, "DP-1"));
  ok = expect(parsePrimary(withPrimary) == "DP-1", "el principal del bloque es el único") && ok;
  ok =
      expect(withPrimary.find("cursor = { default_monitor = \"DP-2\" },") == std::string::npos, "quita la línea suelta")
      && ok;
  ok = expect(withPrimary.find("misc = { vrr = 3 },") != std::string::npos, "conserva el resto de hl.config") && ok;
  ok = expect(
           replaceManagedBlock(withPrimary, serializeBlock(rules, "DP-1")) == withPrimary, "idempotente con principal"
       )
      && ok;
  ok = expect(parseRules(withPrimary) == rules, "las reglas no cambian al agregar el principal") && ok;
  // Sin principal en el bloque, la línea suelta del usuario se respeta.
  ok = expect(parsePrimary(first) == "DP-2", "sin principal no se toca la línea del usuario") && ok;

  // Tamaño lógico con escala y rotación.
  const auto size = logicalSize(custom);
  ok = expect(size.width == 1440 / 1.25 && size.height == 2560 / 1.25, "tamaño lógico rotado y escalado") && ok;

  // Imán: a 30 px del borde derecho de DP-1 se pega; a 200 px no.
  const Rect dp1{.x = 0, .y = 0, .width = 1920, .height = 1080};
  const Rect near = snapRect({.x = 1950, .y = 20, .width = 1920, .height = 1080}, {dp1}, 50.0);
  ok = expect(near.x == 1920 && near.y == 0, "se pega al borde y alinea arriba") && ok;
  const Rect far = snapRect({.x = 2120, .y = 300, .width = 1920, .height = 1080}, {dp1}, 50.0);
  ok = expect(far.x == 2120 && far.y == 300, "lejos no se mueve") && ok;

  std::vector<OutputRule> shifted = moved;
  normalizePositions(shifted);
  ok = expect(shifted[0].y == 1080 && shifted[1].y == 0 && shifted[1].x == 0, "normaliza al origen") && ok;

  // Estado en vivo: modo más cercano y reglas del archivo para lo que el estado no distingue.
  const auto live = parseMonitorsJson(nlohmann::json::parse(R"([{
    "name": "DP-1", "width": 1920, "height": 1080, "refreshRate": 143.99899, "x": 0, "y": 0, "scale": 1,
    "transform": 0, "disabled": false, "vrr": false, "mirrorOf": "none", "currentFormat": "XRGB8888",
    "colorManagementPreset": "srgb", "sdrBrightness": 1, "sdrSaturation": 1,
    "availableModes": ["1920x1080@60.00Hz", "1920x1080@144.00Hz", "1920x1080@119.88Hz"],
    "hardwareDetails": {"hdr": false, "vrrCapable": true}
  }])"));
  ok = expect(live.size() == 1 && live[0].vrrCapable && live[0].mirrorOf.empty(), "lee el JSON de Hyprland") && ok;
  ok = expect(closestMode(live[0]) == "1920x1080@144.00Hz", "modo más cercano a 143.999") && ok;
  auto fileRules = rules;
  fileRules[0].vrr = 2;
  ok = expect(ruleFor(live[0], fileRules).vrr == 2, "vrr sale del archivo") && ok;

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
