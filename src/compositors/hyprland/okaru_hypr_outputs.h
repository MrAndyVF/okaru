#pragma once

// OKARU: lectura, aplicación en vivo y persistencia de monitores en Hyprland (config Lua).
// Estado en vivo: `j/monitors all`. Aplicación: `eval hl.monitor({...})`.
// Persistencia: bloque administrado dentro de ~/.config/hypr/oka/outputs.lua.

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

  // Lo que reporta Hyprland de un monitor.
  struct OutputInfo {
    std::string name;
    std::string description;
    int width = 0;
    int height = 0;
    double refresh = 0.0;
    int x = 0;
    int y = 0;
    double scale = 1.0;
    int transform = 0;
    bool disabled = false;
    bool focused = false;
    bool vrrActive = false;
    bool vrrCapable = false;
    bool hdrCapable = false;
    int bitdepth = 8;
    std::string mirrorOf; // vacío si no espeja
    std::string cm = "srgb";
    double sdrBrightness = 1.0;
    double sdrSaturation = 1.0;
    std::vector<std::string> modes; // "1920x1080@144.00Hz", como los acepta hl.monitor
  };

  // Una regla hl.monitor completa.
  struct OutputRule {
    std::string output;
    std::string mode;
    int x = 0;
    int y = 0;
    double scale = 1.0;
    int transform = 0; // 0-7, wl_output_transform
    int vrr = -1;      // -1 = hereda misc.vrr; 0 off, 1 on, 2 fullscreen, 3 fullscreen con contenido de juego
    int bitdepth = 8;
    std::string cm = "srgb";
    double sdrBrightness = 1.0;
    double sdrSaturation = 1.0;
    std::string mirror; // vacío = sin espejo
    bool disabled = false;
    // Área reservada en píxeles (además de la que piden barras y paneles); solo existe en el archivo.
    int reservedTop = 0;
    int reservedRight = 0;
    int reservedBottom = 0;
    int reservedLeft = 0;

    bool operator==(const OutputRule&) const = default;
  };

  inline constexpr std::string_view kBlockBegin = "-- okaru:monitors:begin";
  inline constexpr std::string_view kBlockEnd = "-- okaru:monitors:end";

  [[nodiscard]] std::vector<OutputInfo> parseMonitorsJson(const nlohmann::json& json);
  [[nodiscard]] std::vector<OutputInfo> queryOutputs(compositors::hyprland::HyprlandRuntime& runtime);

  // Monitor principal (cursor.default_monitor): recibe el cursor y el foco al iniciar. Vacío = sin definir.
  [[nodiscard]] std::string queryPrimary(compositors::hyprland::HyprlandRuntime& runtime);
  [[nodiscard]] std::string parsePrimary(std::string_view lua);

  // Reglas hl.monitor que haya en un texto Lua (bloque administrado o reglas escritas a mano).
  // Ignora la regla comodín (output = "").
  [[nodiscard]] std::vector<OutputRule> parseRules(std::string_view lua);

  // Regla para un monitor: el estado en vivo manda; vrr, bitdepth y cm salen de la regla del archivo
  // si existe, porque el estado en vivo no los distingue (p. ej. vrr=2 inactivo vs 0).
  [[nodiscard]] OutputRule ruleFor(const OutputInfo& info, const std::vector<OutputRule>& fileRules);

  // `explicitDefaults` escribe también los campos en su valor por defecto: hace falta en vivo,
  // porque Hyprland fusiona la regla nueva con la anterior del mismo monitor.
  [[nodiscard]] std::string serializeRule(const OutputRule& rule, bool explicitDefaults = false);

  // Bloque con marcadores, una línea por monitor, la regla comodín y el monitor principal si hay.
  [[nodiscard]] std::string serializeBlock(const std::vector<OutputRule>& rules, std::string_view primary = {});

  // Reemplaza el bloque administrado. Si no hay marcadores (primera vez), el bloque ocupa el lugar
  // de la primera línea hl.monitor y se quitan las demás; si no hay ninguna, va al principio.
  // Si el bloque define el monitor principal, se quita la línea `cursor = { default_monitor = ... }`
  // que haya fuera, porque si va después lo pisaría.
  [[nodiscard]] std::string replaceManagedBlock(std::string_view fileText, std::string_view block);

  [[nodiscard]] std::filesystem::path outputsFilePath();
  [[nodiscard]] std::vector<OutputRule> readFileRules();
  [[nodiscard]] std::string readFilePrimary();

  // Aplica en vivo. Devuelve el error de Hyprland si lo hay.
  [[nodiscard]] std::optional<std::string> applyLive(
      compositors::hyprland::HyprlandRuntime& runtime, const std::vector<OutputRule>& rules,
      std::string_view primary = {}
  );

  // Escribe el bloque en outputs.lua de forma atómica. Devuelve el error si falla.
  [[nodiscard]] std::optional<std::string>
  writeFileRules(const std::vector<OutputRule>& rules, std::string_view primary = {});

  // Tamaño lógico en el escritorio (modo / escala, con ancho y alto cruzados si está rotado 90/270).
  struct LogicalSize {
    double width = 0.0;
    double height = 0.0;
  };
  [[nodiscard]] LogicalSize logicalSize(const OutputRule& rule);

  // Modo "WxH@R.RRHz" de la lista que mejor coincide con el estado en vivo.
  [[nodiscard]] std::string closestMode(const OutputInfo& info);

  // Coloca `moving` pegado al borde más cercano de los demás si está a menos de `threshold`
  // (unidades lógicas) y lo devuelve; los demás no se mueven.
  struct Rect {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
  };
  [[nodiscard]] Rect snapRect(Rect moving, const std::vector<Rect>& others, double threshold);

  // Desplaza todo para que el monitor más arriba a la izquierda quede en 0,0 y redondea a enteros.
  void normalizePositions(std::vector<OutputRule>& rules);

} // namespace okaru::hypr
