#pragma once

// OKARU: Pantallas y Workspaces en Niri. La shell escribe sus propios archivos, ~/.config/niri/okaru/outputs.kdl
// y workspaces.kdl, que el config.kdl del usuario carga con `include optional=true` (Niri ≥ 26.04). Niri vigila
// los archivos incluidos y recarga solo, así que escribir el archivo es aplicar. Cada escritura pasa antes por
// `niri validate`. El estado en vivo sale del IPC (Outputs / Workspaces).

#include "compositors/hyprland/okaru_hypr_outputs.h"

#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace compositors::niri {
  class NiriRuntime;
}

namespace okaru::niri {

  // Pantallas usa las mismas estructuras que con Hyprland. En Niri `vrr` vale 0 apagado, 1 encendido y
  // 2 bajo demanda; bitdepth, cm, sdr*, mirror y reserved* no se usan.
  using okaru::hypr::OutputInfo;
  using okaru::hypr::OutputRule;

  struct NamedWorkspace {
    std::string name;
    std::string output; // vacío = sin open-on-output

    bool operator==(const NamedWorkspace&) const = default;
  };

  // ~/.config/niri/config.kdl (o $NIRI_CONFIG) y la carpeta okaru/ a su lado.
  [[nodiscard]] std::filesystem::path configPath();
  [[nodiscard]] std::filesystem::path outputsPath();
  [[nodiscard]] std::filesystem::path workspacesPath();

  // ¿El config.kdl ya carga los dos archivos de OKARU?
  [[nodiscard]] bool hasOkaruIncludes(std::string_view configText);
  // Texto de config.kdl con las dos líneas include añadidas al final.
  [[nodiscard]] std::string withOkaruIncludes(std::string_view configText);
  // Crea los archivos vacíos, añade los include y valida; si Niri lo rechaza, deja config.kdl como estaba.
  [[nodiscard]] std::optional<std::string> activateIncludes();
  [[nodiscard]] bool includesActive();

  [[nodiscard]] std::string kdlString(std::string_view value);

  // Pantallas.
  [[nodiscard]] std::vector<OutputInfo> parseOutputsJson(const nlohmann::json& json);
  [[nodiscard]] std::vector<OutputInfo> queryOutputs(const compositors::niri::NiriRuntime& runtime);
  // Cada monitor se escribe con su `identity` de `infos` si la tiene (sobrevive a cambiar de puerto); si no, con
  // su conector.
  [[nodiscard]] std::string serializeOutputs(
      const std::vector<OutputRule>& rules, std::string_view primary, const std::vector<OutputInfo>& infos = {}
  );
  // Lee lo que escribe serializeOutputs; `primary` recibe el monitor con focus-at-startup.
  [[nodiscard]] std::vector<OutputRule> parseOutputs(std::string_view kdl, std::string* primary = nullptr);
  // El estado en vivo manda; la VRR sale del archivo, porque en vivo no se distingue "bajo demanda". La regla
  // del archivo se busca por conector o por identidad, y la devuelta siempre usa el conector.
  [[nodiscard]] OutputRule ruleFor(const OutputInfo& info, const std::vector<OutputRule>& fileRules);
  // `rules` y `primary` usan conectores; conserva del archivo las reglas de monitores desconectados.
  [[nodiscard]] std::optional<std::string>
  writeOutputs(const std::vector<OutputRule>& rules, std::string_view primary, const std::vector<OutputInfo>& infos);
  [[nodiscard]] std::vector<OutputRule> readOutputs(std::string* primary = nullptr);

  // Workspaces con nombre, en el orden en que Niri los crea.
  [[nodiscard]] bool isValidWorkspaceName(std::string_view name);
  [[nodiscard]] std::string serializeWorkspaces(const std::vector<NamedWorkspace>& workspaces);
  [[nodiscard]] std::vector<NamedWorkspace> parseWorkspaces(std::string_view kdl);
  [[nodiscard]] std::vector<NamedWorkspace> readWorkspaces();
  // Guarda y aplica: un renombrado se hace primero en vivo (si no, Niri crearía uno nuevo y dejaría el viejo
  // sin nombre); un cambio de monitor mueve el workspace en vivo.
  [[nodiscard]] std::optional<std::string> saveWorkspaces(
      const compositors::niri::NiriRuntime& runtime, const std::vector<NamedWorkspace>& before,
      const std::vector<NamedWorkspace>& after, std::string_view renamedFrom = {}, std::string_view renamedTo = {}
  );

} // namespace okaru::niri
