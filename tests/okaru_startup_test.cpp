// OKARU: autostart XDG, uwsm/env y comandos de hyprland.start.
#include "system/okaru_autostart.h"
#include "system/okaru_uwsm_env.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <print>
#include <unistd.h>

using namespace okaru;

namespace {

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "okaru_startup_test: {}", message);
    }
    return condition;
  }

  void write(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path) << text;
  }

  // Extracto de ~/.config/uwsm/env (2026-11-01).
  constexpr const char* kEnv = R"(# Entorno de la sesión
# Qt usa el tema GTK3
export QT_QPA_PLATFORMTHEME=gtk3
export QT_QPA_PLATFORMTHEME_QT6=gtk3

# NVIDIA (RTX 3060 Ti)
export LIBVA_DRIVER_NAME=nvidia
export XCURSOR_THEME=capitaine-cursors-light
)";

} // namespace

int main() {
  bool ok = true;

  // --- uwsm/env ---
  const auto vars = uwsm::parseEnv(kEnv);
  ok =
      expect(vars.size() == 4 && vars[0].key == "QT_QPA_PLATFORMTHEME" && vars[0].value == "gtk3", "lee exports") && ok;
  ok = expect(vars[0].comment == "Entorno de la sesión Qt usa el tema GTK3", "comentario de arriba") && ok;
  ok = expect(vars[1].comment.empty(), "el comentario es solo de la primera del grupo") && ok;
  ok = expect(vars[2].comment == "NVIDIA (RTX 3060 Ti)", "comentario tras línea en blanco") && ok;
  ok = expect(vars[3].managed && !vars[2].managed, "cursor es solo lectura") && ok;

  const std::string edited = uwsm::setVar(kEnv, "LIBVA_DRIVER_NAME", "nvidia two");
  ok = expect(edited.find("export LIBVA_DRIVER_NAME='nvidia two'\n") != std::string::npos, "edita en su lugar") && ok;
  ok = expect(edited.find("# NVIDIA (RTX 3060 Ti)\nexport LIBVA") != std::string::npos, "conserva comentarios") && ok;
  ok = expect(uwsm::parseEnv(edited)[2].value == "nvidia two", "ida y vuelta con comillas") && ok;
  ok = expect(uwsm::setVar(kEnv, "NEW_VAR", "1").ends_with("export NEW_VAR=1\n"), "agrega al final") && ok;
  ok = expect(uwsm::parseEnv(uwsm::removeVar(kEnv, "QT_QPA_PLATFORMTHEME_QT6")).size() == 3, "quita") && ok;
  ok = expect(uwsm::quote("it's") == "'it'\\''s'", "comilla simple escapada") && ok;
  ok = expect(uwsm::isValidKey("_A1") && !uwsm::isValidKey("1A") && !uwsm::isValidKey("A-B"), "claves") && ok;

  // --- .desktop ---
  const auto keys =
      autostart::parseDesktopEntry("[Desktop Entry]\nName=OpenRGB\nExec=openrgb\n[Desktop Action x]\nName=Otra\n");
  ok = expect(keys.at("Name") == "OpenRGB" && keys.size() == 2, "solo [Desktop Entry]") && ok;
  ok = expect(!autostart::appliesTo({{"OnlyShowIn", "GNOME;KDE;"}}, "Hyprland"), "OnlyShowIn ajeno") && ok;
  ok = expect(autostart::appliesTo({{"OnlyShowIn", "GNOME;Hyprland;"}}, "Hyprland"), "OnlyShowIn propio") && ok;
  ok = expect(!autostart::appliesTo({{"NotShowIn", "hyprland"}}, "Hyprland"), "NotShowIn sin mayúsculas") && ok;
  ok = expect(!autostart::isEnabled({{"X-GNOME-Autostart-enabled", "false"}}), "X-GNOME apagado") && ok;
  const std::string hidden =
      autostart::setKey("[Desktop Entry]\nName=A\n[Desktop Action b]\nExec=b\n", "Hidden", "true");
  ok = expect(
           hidden == "[Desktop Entry]\nName=A\nHidden=true\n[Desktop Action b]\nExec=b\n", "setKey dentro de la sección"
       )
      && ok;
  ok = expect(autostart::idForName("Mi Script!") == "okaru-mi-script.desktop", "id de nombre") && ok;
  ok = expect(
           autostart::unitName("arch-update-tray.desktop") == "app-arch\\x2dupdate\\x2dtray@autostart.service", "unidad"
       )
      && ok;
  const auto states = autostart::parseUnitStates(
      "app-OpenRGB@autostart.service loaded active running OpenRGB\napp-x@autostart.service loaded inactive dead X\n"
  );
  ok = expect(states.at("app-OpenRGB@autostart.service") && !states.at("app-x@autostart.service"), "estados") && ok;

  // Lista unida: la copia del usuario manda; lo que no aplica a Hyprland no sale.
  const auto root = std::filesystem::temp_directory_path() / ("okaru-startup-" + std::to_string(::getpid()));
  write(root / "user/Kde.desktop", "[Desktop Entry]\nHidden=true\n");
  write(root / "user/okaru-mio.desktop", autostart::newEntryText("Mío", "mi-cmd", ""));
  write(root / "sys/Kde.desktop", "[Desktop Entry]\nName=KDE Connect\nExec=kdeconnectd\n");
  write(root / "sys/Gnome.desktop", "[Desktop Entry]\nName=G\nExec=g\nOnlyShowIn=GNOME;\n");
  const auto entries = autostart::listEntries(root / "user", {root / "sys"}, "Hyprland");
  ok = expect(entries.size() == 2, "dos entradas aplican") && ok;
  const auto* kde = entries.size() == 2 ? &entries[0] : nullptr;
  ok = expect(kde != nullptr && kde->id == "Kde.desktop" && kde->user && kde->system && !kde->enabled, "copia oculta")
      && ok;
  ok = expect(entries.size() == 2 && entries[1].ownedByOkaru && entries[1].exec == "mi-cmd", "creada por OKARU") && ok;
  std::filesystem::remove_all(root);

  // --- hyprland.start ---
  const auto cmds = autostart::parseHyprlandStart(
      R"(hl.on("hyprland.start", function()
	hl.exec_cmd("hyprpm reload")
	-- hl.exec_cmd("comentado")
end)
hl.exec_cmd("fuera")
hl.on("hyprland.start", function() hl.exec_cmd("una-linea") end)
hl.bind("X", hl.dsp.exec_cmd("no"))
)",
      "layout.lua"
  );
  ok = expect(cmds.size() == 2 && cmds[0].command == "hyprpm reload" && cmds[1].command == "una-linea", "start") && ok;
  ok = expect(cmds[0].file == "layout.lua", "archivo de origen") && ok;

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
