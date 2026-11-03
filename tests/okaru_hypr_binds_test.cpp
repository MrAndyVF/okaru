// OKARU: lectura, categorías y escritura de atajos (binds.lua / user-binds.lua).
#include "compositors/hyprland/okaru_hypr_binds.h"

#include <cstdlib>
#include <print>
#include <string>

using namespace okaru::hypr;

namespace {

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "okaru_hypr_binds_test: {}", message);
    }
    return condition;
  }

  // Extracto con la forma de ~/.config/hypr/oka/binds.lua (2026-11-01).
  constexpr const char* kBinds = R"(-- Atajos
local ipc = "noctalia msg "

-- === Aplicaciones ===
hl.bind("SUPER + B", hl.dsp.exec_cmd("google-chrome-stable --ozone-platform=wayland"))

-- === OKARU ===
hl.bind("SUPER + space", hl.dsp.exec_cmd(ipc .. "panel-toggle launcher"))
hl.bind("SUPER + Period", hl.dsp.exec_cmd(ipc .. [[panel-toggle launcher "/emo "]]), { description = "Emoji Picker" })

-- === Audio ===
hl.bind("XF86AudioLowerVolume", hl.dsp.exec_cmd(ipc .. "volume-down 5"), { locked = true, repeating = true })

-- === Foco ===
hl.bind("SUPER + H", hl.dsp.focus({ direction = "l" }))
hl.bind("SUPER + mouse:272", hl.dsp.window.drag(), { mouse = true, description = "Move window" })
hl.bind("SUPER + SHIFT + E", hl.dsp.exit())
hl.bind("F7", hl.dsp.global("discord:discord-toggle-deafen"))
for n = 1, 9 do
  hl.bind("SUPER + " .. n, function() end)
end
)";

} // namespace

int main() {
  bool ok = true;
  const auto binds = parseBinds(kBinds, BindFile::Binds);
  ok = expect(binds.size() == 8, "ocho atajos (el del bucle no cuenta)") && ok;

  // Reescribir sin cambios da el mismo texto.
  const std::string text(kBinds);
  for (const auto& b : binds) {
    ok = expect(serializeBind(b.keys, b.dispatcher, b.options) == text.substr(b.start, b.end - b.start), "mismo texto")
        && ok;
  }

  ok = expect(
           categoryOf(binds[0]) == BindCategory::User && actionOf(binds[0]).kind == BindAction::Kind::Command,
           "app -> User"
       )
      && ok;
  ok = expect(
           categoryOf(binds[1]) == BindCategory::System && actionOf(binds[1]).text == "panel-toggle launcher",
           "shell -> Sistema"
       )
      && ok;
  ok =
      expect(
          categoryOf(binds[2]) == BindCategory::System && actionOf(binds[2]).text == R"(panel-toggle launcher "/emo ")",
          "cadena larga de Lua"
      )
      && ok;
  ok = expect(
           categoryOf(binds[3]) == BindCategory::Media && binds[3].options["repeating"] == true, "volumen -> Multimedia"
       )
      && ok;
  ok = expect(categoryOf(binds[4]) == BindCategory::Input && binds[4].section == "Foco", "foco -> Entrada, con sección")
      && ok;
  ok = expect(categoryOf(binds[5]) == BindCategory::Input, "arrastrar con ratón -> Entrada") && ok;
  ok = expect(categoryOf(binds[6]) == BindCategory::System, "cerrar sesión -> Sistema") && ok;
  ok = expect(categoryOf(binds[7]) == BindCategory::User, "global de una app -> User") && ok;
  ok = expect(
           categoryOf(Bind{.keys = "SUPER + X", .dispatcher = "hl.dsp.window.close()", .file = BindFile::UserBinds})
               == BindCategory::User,
           "todo user-binds.lua es User"
       )
      && ok;

  // Escritura.
  ok =
      expect(shellDispatcher("settings-toggle") == R"(hl.dsp.exec_cmd(ipc .. "settings-toggle"))", "acción de la shell")
      && ok;
  ok = expect(commandDispatcher("zeditor") == R"(hl.dsp.exec_cmd("zeditor"))", "comando") && ok;
  nlohmann::ordered_json options = nlohmann::ordered_json::object();
  options["locked"] = true;
  ok = expect(
           serializeBind("SUPER + Z", commandDispatcher("zeditor"), options)
               == R"(hl.bind("SUPER + Z", hl.dsp.exec_cmd("zeditor"), { locked = true }))",
           "atajo con opciones"
       )
      && ok;

  ok = expect(normalizeKeys("super+shift+c") == normalizeKeys("SHIFT + SUPER + C"), "misma combinación") && ok;
  ok = expect(normalizeKeys("SUPER + C") != normalizeKeys("SUPER + SHIFT + C"), "combinaciones distintas") && ok;

  // Subcategorías: los encabezados del usuario se reconocen en español o inglés.
  ok = expect(sectionIdOf("Mover ventanas / columnas") == "move" && sectionIdOf("Focus") == "focus", "subcategorías")
      && ok;
  ok = expect(sectionIdOf("Workspaces (siguiente/anterior y Mod+N)") == "workspaces", "ignora el paréntesis") && ok;
  ok = expect(sectionIdOf("Cosas mías").empty(), "encabezado desconocido") && ok;

  // Insertar al final de una sección existente (antes de la siguiente) y crear una nueva.
  const std::string line = R"(hl.bind("SUPER + J", hl.dsp.focus({ direction = "d" })))";
  const std::string inFocus = insertInSection(kBinds, "focus", "Foco", line);
  const auto focusBinds = parseBinds(inFocus, BindFile::Binds);
  ok = expect(
           focusBinds.size() == 9 && focusBinds[8].section == "Foco" && focusBinds[8].keys == "SUPER + J",
           "va al final de Foco"
       )
      && ok;
  ok = expect(inFocus.find(line) < inFocus.find("for n = 1, 9 do"), "antes de lo que sigue") && ok;
  const std::string withNew = insertInSection(kBinds, "brightness", "Brillo", line);
  ok = expect(withNew.ends_with("\n-- === Brillo ===\n" + line + "\n"), "crea la sección") && ok;

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
