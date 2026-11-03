// OKARU: lectura y edición en su lugar de hypr/oka/windowrules.lua.
#include "compositors/hyprland/okaru_hypr_rules.h"

#include <cstdlib>
#include <print>
#include <string>

using namespace okaru::hypr;

namespace {

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "okaru_hypr_rules_test: {}", message);
    }
    return condition;
  }

  // Extracto con la forma de ~/.config/hypr/oka/windowrules.lua (2026-11-01).
  constexpr const char* kFile = R"(-- Reglas de ventana

-- Nautilus
hl.window_rule({ match = { class = "^(org\\.gnome\\.Nautilus)$" }, float = true, size = { 1300, 800 } })

-- Chats y diálogos de Steam: Steam recuerda tamaños.
-- Solo con título
hl.window_rule({ match = { class = "^steam$", title = "^.+$", float = true }, max_size = { 1000, 850 }, center = true })
-- local x = hl.window_rule({ esto no es una regla })
hl.layer_rule({
	name = "shell",
	match = { namespace = "^(okaru|okaru)-(bar-.+|osd)$" },
	no_anim = true,
})
)";

} // namespace

int main() {
  bool ok = true;

  const auto rules = parseRulesFile(kFile);
  ok = expect(rules.size() == 3, "tres reglas (la comentada no cuenta)") && ok;
  ok = expect(rules[0].title == "Nautilus" && rules[0].description.empty() && rules[0].table["size"][1] == 800, "comentario y tamaño") && ok;
  ok = expect(rules[0].table["match"]["class"] == R"(^(org\.gnome\.Nautilus)$)", "escapes de la regex") && ok;
  ok = expect(
           rules[1].title == "Chats y diálogos de Steam: Steam recuerda tamaños." && rules[1].description == "Solo con título",
           "comentario de dos líneas"
       )
      && ok;
  ok = expect(
           rules[2].kind == RuleKind::Layer && rules[2].table["name"] == "shell" && rules[2].table["no_anim"] == true,
           "regla de capa en varias líneas"
       )
      && ok;

  // Reescribir una regla sin cambios conserva el orden de los campos y las regex.
  const std::string same = serializeRuleCall(rules[1].kind, rules[1].table);
  ok =
      expect(
          same
              == R"(hl.window_rule({ match = { class = "^steam$", title = "^.+$", float = true }, max_size = { 1000, 850 }, center = true }))",
          "misma regla, mismo texto"
      )
      && ok;

  // Editar en su lugar: el resto del archivo no cambia.
  auto edited = rules[0].table;
  edited["center"] = true;
  const std::string afterEdit = replaceRule(kFile, rules[0], serializeRuleCall(RuleKind::Window, edited));
  const auto reread = parseRulesFile(afterEdit);
  ok = expect(
           reread.size() == 3 && reread[0].table["center"] == true && reread[0].title == "Nautilus",
           "edición en su lugar"
       )
      && ok;
  ok = expect(afterEdit.find("-- Chats y diálogos de Steam") != std::string::npos, "conserva los otros comentarios")
      && ok;

  // Quitar se lleva el comentario de la regla.
  const std::string afterRemove = removeRule(kFile, rules[1]);
  ok = expect(parseRulesFile(afterRemove).size() == 2, "quita la regla") && ok;
  ok = expect(afterRemove.find("Steam") == std::string::npos, "quita su comentario") && ok;
  ok = expect(afterRemove.find("-- Nautilus") != std::string::npos, "deja los demás") && ok;

  // Agregar al final, con comentario.
  nlohmann::ordered_json fresh = nlohmann::ordered_json::object();
  fresh["match"] = {{"class", exactRegex("org.kde.dolphin")}};
  fresh["float"] = true;
  const std::string afterAppend = appendRule(kFile, "Dolphin", "Gestor de archivos de KDE", serializeRuleCall(RuleKind::Window, fresh));
  const auto appended = parseRulesFile(afterAppend);
  ok = expect(appended.size() == 4 && appended.back().title == "Dolphin" && appended.back().description == "Gestor de archivos de KDE", "agrega al final") && ok;
  ok = expect(appended.back().table["match"]["class"] == R"(^(org\.kde\.dolphin)$)", "regex exacta") && ok;

  ok =
      expect(
          !parseLuaTable("{ float = }").has_value() && !parseLuaTable("{ a = 1 } x").has_value(), "rechaza Lua inválido"
      )
      && ok;

  // Cambiar título y descripción de una regla que ya tenía dos líneas de comentario.
  const std::string renamed = replaceRuleWithComment(
      kFile, rules[1], "Steam", "Chats y diálogos acotados", serializeRuleCall(rules[1].kind, rules[1].table)
  );
  const auto renamedRules = parseRulesFile(renamed);
  ok = expect(
           renamedRules.size() == 3 && renamedRules[1].title == "Steam"
               && renamedRules[1].description == "Chats y diálogos acotados",
           "título y descripción nuevos"
       )
      && ok;
  ok = expect(
           renamed.find("recuerda tamaños") == std::string::npos && renamed.find("-- Nautilus") != std::string::npos,
           "reemplaza solo el comentario de esa regla"
       )
      && ok;
  // Sin título ni descripción, el comentario desaparece.
  const auto bare =
      parseRulesFile(replaceRuleWithComment(kFile, rules[0], "", "", serializeRuleCall(rules[0].kind, rules[0].table)));
  ok = expect(bare.size() == 3 && bare[0].title.empty(), "sin comentario") && ok;

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
