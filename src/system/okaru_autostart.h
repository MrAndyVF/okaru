#pragma once

// OKARU: aplicaciones al iniciar (XDG autostart, que uwsm arranca como app-<id>@autostart.service) y
// comandos que Hyprland lanza al arrancar. Nunca escribe fuera de ~/.config/autostart: para apagar una
// entrada del sistema se crea una copia del usuario con Hidden=true, como manda la especificación.

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace okaru::autostart {

  using Keys = std::map<std::string, std::string>; // claves de [Desktop Entry]

  struct Entry {
    std::string id; // nombre del archivo, "OpenRGB.desktop"
    std::string name;
    std::string exec;
    std::string icon;
    bool user = false;         // hay archivo en ~/.config/autostart
    bool system = false;       // hay archivo en /etc/xdg/autostart (o XDG_CONFIG_DIRS)
    bool enabled = true;       // ni Hidden=true ni X-GNOME-Autostart-enabled=false
    bool ownedByOkaru = false; // la creó OKARU ("okaru-*.desktop"): se puede quitar
  };

  struct HyprlandStartCommand {
    std::string command;
    std::string file; // "cursor.lua"
  };

  [[nodiscard]] Keys parseDesktopEntry(std::string_view text);
  // Si la entrada aplica a este escritorio (OnlyShowIn/NotShowIn contra XDG_CURRENT_DESKTOP).
  [[nodiscard]] bool appliesTo(const Keys& keys, std::string_view currentDesktops);
  [[nodiscard]] bool isEnabled(const Keys& keys);
  // Cambia o agrega una clave dentro de [Desktop Entry], conservando el resto del archivo.
  [[nodiscard]] std::string setKey(std::string_view text, std::string_view key, std::string_view value);
  // "Mi script" -> "okaru-mi-script.desktop"
  [[nodiscard]] std::string idForName(std::string_view name);
  [[nodiscard]] std::string newEntryText(std::string_view name, std::string_view exec, std::string_view icon);

  [[nodiscard]] std::filesystem::path userDir();
  [[nodiscard]] std::vector<std::filesystem::path> systemDirs();
  [[nodiscard]] std::vector<Entry> listEntries(
      const std::filesystem::path& user, const std::vector<std::filesystem::path>& system, std::string_view desktops
  );
  [[nodiscard]] std::vector<Entry> listEntries();

  // Devuelven el error, o nullopt si salió bien.
  [[nodiscard]] std::optional<std::string> setEnabled(const Entry& entry, bool enabled);
  [[nodiscard]] std::optional<std::string> create(std::string_view name, std::string_view exec, std::string_view icon);
  [[nodiscard]] std::optional<std::string> remove(const Entry& entry);

  // Nombre de la unidad que uwsm crea para una entrada: "app-OpenRGB@autostart.service".
  [[nodiscard]] std::string unitName(std::string_view id);
  // Unidades activas según `systemctl --user list-units` (salida --plain --no-legend).
  [[nodiscard]] std::map<std::string, bool> parseUnitStates(std::string_view listUnits);
  [[nodiscard]] std::map<std::string, bool> queryUnitStates();

  // hl.exec_cmd(...) dentro de hl.on("hyprland.start", ...) en los .lua de `dir` (sin subcarpetas).
  [[nodiscard]] std::vector<HyprlandStartCommand> parseHyprlandStart(std::string_view lua, std::string_view file);
  [[nodiscard]] std::vector<HyprlandStartCommand> hyprlandStartCommands();

} // namespace okaru::autostart
