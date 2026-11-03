#pragma once

// OKARU: atajos de hypr/oka/binds.lua y hypr/oka/user-binds.lua. Cada `hl.bind(...)` de una línea
// se edita en su lugar; los atajos con lógica Lua (bucles, funciones) no se tocan.

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace compositors::hyprland {
  class HyprlandRuntime;
}

namespace okaru::hypr {

  enum class BindCategory : std::uint8_t { System, Media, Input, User, Other };
  enum class BindFile : std::uint8_t { Binds, UserBinds };

  struct Bind {
    std::string keys;       // "SUPER + SHIFT + C"
    std::string dispatcher; // expresión Lua tal cual: hl.dsp.exec_cmd(ipc .. "panel-toggle launcher")
    nlohmann::ordered_json options = nlohmann::ordered_json::object(); // { locked = true, description = "..." }
    BindFile file = BindFile::Binds;
    std::string section; // encabezado "-- === Audio ===" más cercano, si hay
    std::size_t start = 0;
    std::size_t end = 0;
  };

  // Qué hace el atajo, si es una de las formas conocidas.
  struct BindAction {
    enum class Kind : std::uint8_t { Shell, Command, Other } kind = Kind::Other;
    std::string text; // comando de la shell ("panel-toggle launcher") o comando del sistema ("kitty")
  };

  [[nodiscard]] std::vector<Bind> parseBinds(std::string_view text, BindFile file);
  [[nodiscard]] BindAction actionOf(const Bind& bind);
  [[nodiscard]] BindCategory categoryOf(const Bind& bind);

  // `hl.bind("KEYS", <acción>, { opciones })`; la acción de la shell usa la variable `ipc` de binds.lua.
  [[nodiscard]] std::string shellDispatcher(std::string_view command);
  [[nodiscard]] std::string commandDispatcher(std::string_view command);
  [[nodiscard]] std::string
  serializeBind(std::string_view keys, std::string_view dispatcher, const nlohmann::ordered_json& options);

  // "super+shift+c" y "SHIFT + SUPER + C" son la misma combinación.
  [[nodiscard]] std::string normalizeKeys(std::string_view keys);

  // Subcategorías (las secciones "-- === Nombre ===" de los archivos), con los nombres con que se
  // reconocen en español e inglés. La primera de cada categoría es la que se propone al crear.
  struct BindSection {
    std::string_view id;
    BindCategory category;
    std::vector<std::string_view> aliases; // en minúsculas
  };
  [[nodiscard]] const std::vector<BindSection>& bindSections();
  // Id de la subcategoría de un encabezado ("Mover ventanas / columnas" -> "move"); vacío si no se conoce.
  [[nodiscard]] std::string sectionIdOf(std::string_view header);

  // Inserta `line` al final de la sección `sectionId` del texto; si no existe, agrega la sección al
  // final con el encabezado `header`.
  [[nodiscard]] std::string
  insertInSection(std::string_view text, std::string_view sectionId, std::string_view header, std::string_view line);

  [[nodiscard]] std::string readBindsFile(BindFile file);
  [[nodiscard]] std::vector<Bind> readAllBinds();

  // Escribe los archivos que cambiaron, recarga Hyprland y, si reporta un error en ellos, los deja
  // como estaban. `before`/`after` van en el orden {binds, user-binds}.
  [[nodiscard]] std::optional<std::string> writeBindFiles(
      compositors::hyprland::HyprlandRuntime& runtime, const std::vector<std::string>& before,
      const std::vector<std::string>& after
  );

} // namespace okaru::hypr
