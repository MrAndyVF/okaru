#pragma once

// OKARU: reglas de ventana y de capa de hypr/oka/windowrules.lua. Cada regla se edita en su lugar:
// solo se reemplaza el texto de esa llamada, y los comentarios y el orden del archivo se conservan.

#include <cstddef>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace compositors::hyprland {
  class HyprlandRuntime;
}

namespace okaru::hypr {

  enum class RuleKind : std::uint8_t { Window, Layer };

  struct Rule {
    RuleKind kind = RuleKind::Window;
    nlohmann::ordered_json table = nlohmann::ordered_json::object(); // la tabla Lua como JSON (objetos y listas)
    std::string title;            // primera línea del comentario de arriba, sin "-- "
    std::string description;      // las demás líneas del comentario, unidas
    std::size_t commentStart = 0; // dónde empieza el comentario (o la regla, si no tiene)
    std::size_t start = 0;        // rango de la llamada en el texto
    std::size_t end = 0;
  };

  // Tabla Lua literal (strings, números, booleanos, tablas) -> JSON. nullopt si no se entiende.
  [[nodiscard]] std::optional<nlohmann::ordered_json> parseLuaTable(std::string_view text);
  // JSON -> tabla Lua en una línea, con el mismo formato que usa el archivo.
  [[nodiscard]] std::string toLuaTable(const nlohmann::ordered_json& value);

  // Todas las llamadas hl.window_rule / hl.layer_rule del texto, en orden. Las que no se puedan
  // leer se omiten (y por lo tanto no se pueden editar desde OKARU, pero tampoco se tocan).
  [[nodiscard]] std::vector<Rule> parseRulesFile(std::string_view text);
  [[nodiscard]] std::string serializeRuleCall(RuleKind kind, const nlohmann::ordered_json& table);

  // Reemplaza la regla (por su rango) o la quita con su comentario. Agregar va al final.
  [[nodiscard]] std::string replaceRule(std::string_view text, const Rule& rule, std::string_view call);
  [[nodiscard]] std::string removeRule(std::string_view text, const Rule& rule);
  [[nodiscard]] std::string
  appendRule(std::string_view text, std::string_view title, std::string_view description, std::string_view call);
  // Igual que replaceRule, y además reemplaza el comentario de encima por título y descripción.
  [[nodiscard]] std::string replaceRuleWithComment(
      std::string_view text, const Rule& rule, std::string_view title, std::string_view description,
      std::string_view call
  );

  // Auxiliares de lectura de Lua, compartidas con los atajos.
  [[nodiscard]] std::optional<std::size_t> luaCallEnd(std::string_view text, std::size_t openParen);
  [[nodiscard]] std::size_t luaLineStart(std::string_view text, std::size_t pos);
  [[nodiscard]] std::optional<std::string> writeFileAtomic(const std::filesystem::path& path, const std::string& text);

  [[nodiscard]] std::filesystem::path rulesFilePath();
  [[nodiscard]] std::string readRulesText();

  // Escribe el archivo, recarga Hyprland y, si reporta un error en windowrules.lua, deja el archivo
  // como estaba y devuelve el error.
  [[nodiscard]] std::optional<std::string>
  writeRulesText(compositors::hyprland::HyprlandRuntime& runtime, const std::string& previous, const std::string& next);

  // Ventanas abiertas, para crear reglas a partir de ellas.
  struct OpenWindow {
    std::string cls;
    std::string title;
  };
  [[nodiscard]] std::vector<OpenWindow> queryOpenWindows(compositors::hyprland::HyprlandRuntime& runtime);

  // "^(org\\.gnome\\.Nautilus)$" para que coincida exactamente con el texto.
  [[nodiscard]] std::string exactRegex(std::string_view text);

} // namespace okaru::hypr
