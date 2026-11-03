#pragma once

// OKARU: estilos de animación predefinidos para Hyprland. Se guardan como el bloque
// okaru:animations al final de hypr/oka/layout.lua (va después de las animaciones del usuario,
// así que gana); sin bloque vale lo que el usuario tenga escrito.

#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace compositors::hyprland {
  class HyprlandRuntime;
}

namespace okaru::hypr {

  struct AnimCurve {
    std::string name;
    bool spring = false;
    double x0 = 0.0, y0 = 0.0, x1 = 1.0, y1 = 1.0;     // bezier
    double stiffness = 0.0, damping = 0.0, mass = 1.0; // resorte
  };

  struct AnimLeaf {
    std::string leaf;
    bool enabled = true;
    double speed = 3.0; // décimas de segundo (solo bezier)
    std::string curve;  // nombre de una AnimCurve
    std::string style;  // "popin 80%", "slidevert", ...

    bool operator==(const AnimLeaf&) const = default;
  };

  struct AnimPreset {
    std::string id;
    std::vector<AnimCurve> curves;
    std::vector<AnimLeaf> leaves;
  };

  // Estado guardado: estilo, factor de duración y estilo del cambio de escritorio.
  // preset vacío o "custom" = Personalizado: la config del usuario más `custom` (solo las animaciones
  // editadas desde OKARU). Sin ediciones no hay bloque.
  struct AnimChoice {
    std::string preset;                   // vacío o "custom" = Personalizado
    double duration = 1.0;                // 0.5 - 2.0
    std::string workspaces = "slidevert"; // "slidevert", "slide" o "fade"
    std::vector<AnimLeaf> custom;

    bool operator==(const AnimChoice&) const = default;
  };

  [[nodiscard]] const std::vector<AnimPreset>& animationPresets();
  [[nodiscard]] const AnimPreset* findPreset(std::string_view id);
  [[nodiscard]] bool isCustom(const AnimChoice& choice);

  // Animación efectiva en vivo, con la herencia de Hyprland resuelta (windowsIn <- windows <- global).
  struct LiveLeaf {
    std::string leaf;
    bool enabled = true;
    double speed = 0.0;
    std::string curve; // nombre, sin el prefijo "spring:"
    bool spring = false;
    std::string style;
    bool overridden = false; // definida explícitamente (no heredada)
  };
  struct LiveAnimations {
    std::vector<LiveLeaf> leaves;
    std::vector<std::string> beziers; // curvas bezier definidas en Hyprland
    std::vector<std::string> springs; // resortes en uso
  };
  [[nodiscard]] LiveAnimations parseAnimationsJson(const nlohmann::json& json);
  [[nodiscard]] LiveAnimations queryAnimations(compositors::hyprland::HyprlandRuntime& runtime);
  // Curvas de todos los estilos de OKARU (siempre se definen en el bloque Personalizado).
  [[nodiscard]] std::vector<AnimCurve> okaruCurves();

  // Aplica la duración (bezier: speed·k; resorte: stiffness/k², damping/k) y el estilo de escritorio.
  [[nodiscard]] AnimPreset resolvePreset(const AnimChoice& choice);

  [[nodiscard]] std::string serializeAnimations(const AnimPreset& preset);
  [[nodiscard]] std::string serializeAnimationBlock(const AnimChoice& choice);
  [[nodiscard]] AnimChoice parseAnimationBlock(std::string_view lua);
  [[nodiscard]] std::string replaceAnimationBlock(std::string_view fileText, const AnimChoice& choice);

  [[nodiscard]] AnimChoice readAnimationChoice();
  [[nodiscard]] std::optional<std::string> writeAnimationChoice(const AnimChoice& choice);
  [[nodiscard]] std::optional<std::string>
  applyAnimationsLive(compositors::hyprland::HyprlandRuntime& runtime, const AnimChoice& choice);

  // Vista previa: progreso (0 = inicio, 1 = destino; puede pasarse si rebota) a `ms` del comienzo,
  // y cuánto dura la animación de esa hoja.
  [[nodiscard]] double curveProgress(const AnimCurve& curve, double ms, double durationMs);
  [[nodiscard]] double leafDurationMs(const AnimPreset& preset, const AnimLeaf& leaf);

  // Curva normalizada (t 0..1) del estilo para las animaciones de la shell: la de windowsMove.
  // Vacía si el estilo no existe o es "minimal" (ahí la shell usa movimiento reducido).
  [[nodiscard]] std::function<float(float)> shellEasing(std::string_view style);

} // namespace okaru::hypr
