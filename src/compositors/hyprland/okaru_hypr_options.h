#pragma once

// OKARU: opciones de Hyprland (hl.config) leídas de `j/descriptions` y guardadas como un bloque de
// overrides al final de un archivo de hypr/oka/. El bloque va al final para ganar sobre lo anterior
// sin tocar el resto del archivo; "restablecer" quita la línea del bloque.

#include <filesystem>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace compositors::hyprland {
  class HyprlandRuntime;
}

namespace okaru::hypr {

  enum class OptionKind : std::uint8_t { Bool, Int, Float, String, Enum, Gaps };

  struct OptionDesc {
    std::string name; // "decoration:blur:passes"
    std::string description;
    OptionKind kind = OptionKind::String;
    nlohmann::json defaultValue;
    nlohmann::json current;
    std::optional<double> min;
    std::optional<double> max;
    std::vector<std::pair<std::string, int>> choices; // para Enum, en orden de valor
  };

  using OptionValues = std::map<std::string, nlohmann::json>; // nombre -> valor

  [[nodiscard]] std::vector<OptionDesc> parseDescriptionsJson(const nlohmann::json& json);
  [[nodiscard]] std::vector<OptionDesc> queryOptions(compositors::hyprland::HyprlandRuntime& runtime);

  // `hl.config({ decoration = { blur = { passes = 3 } } })`
  [[nodiscard]] std::string serializeOption(std::string_view name, const nlohmann::json& value);
  // `bases` guarda, por opción, el valor que tenía antes del override (como comentario `-- base:`),
  // para saber cuándo el usuario volvió al valor original aunque se haya reiniciado.
  [[nodiscard]] std::string serializeOptionsBlock(const OptionValues& values, const OptionValues& bases = {});

  // Lee el bloque okaru:options (solo el formato que escribe serializeOptionsBlock).
  [[nodiscard]] OptionValues parseOptionsBlock(std::string_view lua, OptionValues* bases = nullptr);

  // Reemplaza el bloque, o lo agrega al final si no existe. Con `values` vacío lo quita.
  [[nodiscard]] std::string
  replaceOptionsBlock(std::string_view fileText, const OptionValues& values, const OptionValues& bases = {});

  // Reemplaza en su lugar el bloque entre los marcadores, o lo agrega al final si no existe.
  // Con `block` vacío lo quita (junto con la línea en blanco que lo separaba).
  [[nodiscard]] std::string replaceTrailingBlock(
      std::string_view fileText, std::string_view beginMarker, std::string_view endMarker, std::string_view block
  );

  [[nodiscard]] std::filesystem::path okaFilePath(std::string_view file); // "layout" -> .../hypr/oka/layout.lua
  [[nodiscard]] OptionValues readOptionOverrides(const std::filesystem::path& path, OptionValues* bases = nullptr);
  [[nodiscard]] std::optional<std::string>
  writeOptionOverrides(const std::filesystem::path& path, const OptionValues& values, const OptionValues& bases = {});

  // Aplica una opción en vivo (eval). Devuelve el error de Hyprland si lo hay.
  [[nodiscard]] std::optional<std::string>
  applyOption(compositors::hyprland::HyprlandRuntime& runtime, std::string_view name, const nlohmann::json& value);

  // Igualdad de valores tal como los reporta Hyprland y como se escriben: "8 8 8 8" == 8, 0.9 == 0.90000001.
  [[nodiscard]] bool sameOptionValue(const nlohmann::json& a, const nlohmann::json& b);

  // "4 4 4 4" -> 4; "4 8 4 8" -> nullopt (no uniforme). Hyprland reporta los gaps como CSS.
  [[nodiscard]] std::optional<int> uniformGap(const nlohmann::json& value);

} // namespace okaru::hypr
