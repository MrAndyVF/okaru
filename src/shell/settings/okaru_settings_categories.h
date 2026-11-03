#pragma once

// OKARU: la barra lateral de settings se organiza en módulos (inspirado en dux-shell): cada módulo agrupa
// una o varias secciones de OKARU, que se eligen con un selector horizontal dentro del módulo. Los
// módulos se separan en bloques. Vive en un archivo propio para que los merges
// de upstream no choquen con él.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace settings {
  enum class SettingsSection : std::uint8_t;

  struct SettingsModuleDef {
    std::string_view key; // "system"; etiqueta settings.navigation.modules.<key>, frase <key>-hint
    std::string_view glyph;
    int block = 0;                         // bloques separados en la barra lateral
    std::vector<SettingsSection> sections; // en el selector, en este orden
  };

  // Un módulo con las secciones que están visibles ahora.
  struct SettingsModuleGroup {
    const SettingsModuleDef* module = nullptr;
    std::vector<SettingsSection> sections;

    [[nodiscard]] bool contains(SettingsSection section) const;
    [[nodiscard]] std::size_t size() const noexcept { return sections.size(); }
  };

  [[nodiscard]] std::span<const SettingsModuleDef> settingsModules();
  [[nodiscard]] std::string settingsModuleLabelKey(const SettingsModuleDef& module);
  [[nodiscard]] std::string settingsModuleHintKey(const SettingsModuleDef& module);

  // Reparte `sections` (las visibles) por módulo. `Bar` se incluye siempre. Una sección sin módulo (p. ej.
  // una nueva de upstream) va a Avanzado para que nunca desaparezca. Sin módulos vacíos.
  [[nodiscard]] std::vector<SettingsModuleGroup> groupSettingsModules(std::span<const SettingsSection> sections);
  [[nodiscard]] std::optional<SettingsModuleGroup>
  moduleForSection(std::span<const SettingsSection> sections, SettingsSection section);

  // Última subsección visitada de cada módulo (id de sección), para volver a ella al entrar al módulo.
  [[nodiscard]] std::string lastModuleSection(std::string_view moduleKey);
  void rememberModuleSection(std::string_view moduleKey, std::string sectionId);

} // namespace settings
