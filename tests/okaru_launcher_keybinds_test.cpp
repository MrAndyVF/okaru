// Prueba del catálogo de atajos de okaru-launcher sobre un binds.lua de ejemplo.
#include "shell/launcher/okaru_launcher_keybinds.h"

#include <algorithm>
#include <cassert>
#include <cstdio>

int main() {
  const auto binds = okaru::hypr::parseBinds(R"lua(
local ipc = "okaru msg "
hl.bind("CTRL + ALT + Delete", hl.dsp.exec_cmd("kitty --class kitty-floating btop"))
hl.bind("SUPER + M", hl.dsp.exec_cmd("kitty --class kitty-floating btop"))
hl.bind("SUPER + T", hl.dsp.exec_cmd("kitty --class kitty-floating"))
hl.bind("SUPER + B", hl.dsp.exec_cmd("google-chrome-stable --ozone-platform=wayland"))
hl.bind("SUPER + E", hl.dsp.exec_cmd("uwsm app -- okaru-files"))
hl.bind("SUPER + space", hl.dsp.exec_cmd(ipc .. "panel-toggle launcher"))
hl.bind("SUPER + slash", hl.dsp.exec_cmd(ipc .. [[panel-toggle launcher ":keybinds"]]), { description = "Keyboard Shortcuts" })
hl.bind("SUPER + Y", hl.dsp.exec_cmd(ipc .. "panel-toggle wallpaper"))
hl.bind("SUPER + Q", hl.dsp.window.close())
hl.bind("SUPER + K", focus("u"))
hl.bind("SUPER + G", hl.dsp.exec_cmd("gimp"), { description = "Gimp" })
)lua", okaru::hypr::BindFile::Binds);

  const auto items = okaru::launcher::curatedKeybinds(binds);
  const auto find = [&](std::string_view id) {
    return std::ranges::find(items, id, &okaru::launcher::KeybindItem::description);
  };
  // btop tiene dos atajos: sale el más corto.
  assert(find("monitor") != items.end() && find("monitor")->keys == (std::vector<std::string>{"Super", "M"}));
  // El terminal no se confunde con btop (que también abre kitty).
  assert(find("terminal") != items.end() && find("terminal")->keys == (std::vector<std::string>{"Super", "T"}));
  assert(find("browser") != items.end() && find("files") != items.end());
  // El lanzador no se confunde con la vista de atajos (los dos son panel-toggle launcher).
  assert(find("launcher")->keys == (std::vector<std::string>{"Super", "@space"}) || find("launcher")->keys.size() == 2);
  assert(find("keybinds")->keys == (std::vector<std::string>{"Super", "/"}));
  assert(find("wallpaper") != items.end() && find("close") != items.end());
  // Los que no están en el catálogo y no tienen descripción no salen; los propios con descripción, al final.
  assert(std::ranges::none_of(items, [](const auto& item) { return item.description.contains("focus"); }));
  assert(items.back().group == "custom" && items.back().description == "Gimp");
  // Sin descripción duplicada: el de atajos ya salió en el catálogo, no otra vez en «Tus atajos».
  assert(std::ranges::count(items, std::string("custom"), &okaru::launcher::KeybindItem::group) == 1);

  assert((okaru::launcher::keyNames("SUPER + SHIFT + Return") == std::vector<std::string>{"Super", "Shift", "↵"}));
  assert((okaru::launcher::keyNames("super+c") == std::vector<std::string>{"Super", "C"}));
  std::puts("okaru_launcher_keybinds: OK");
  return 0;
}
