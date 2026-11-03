// OKARU: cada sección visible de settings aparece exactamente una vez en un módulo, Home va primero y
// los módulos vacíos no se muestran.
#include "shell/settings/okaru_settings_categories.h"
#include "shell/settings/settings_registry.h"

#include <algorithm>
#include <cstdlib>
#include <print>

using settings::SettingsSection;

namespace {
  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "okaru_settings_categories_test: {}", message);
    }
    return condition;
  }
} // namespace

int main() {
  bool ok = true;
  std::vector<SettingsSection> sidebar;
  for (const auto& descriptor : settings::settingsSectionDescriptors()) {
    if (descriptor.sidebar) {
      sidebar.push_back(descriptor.section);
    }
  }
  const auto groups = settings::groupSettingsModules(sidebar);
  std::vector<SettingsSection> flat;
  for (const auto& g : groups) {
    flat.insert(flat.end(), g.sections.begin(), g.sections.end());
  }
  for (const auto section : sidebar) {
    ok = expect(std::ranges::count(flat, section) == 1, "una sección visible no aparece exactamente una vez") && ok;
  }
  ok = expect(std::ranges::count(flat, SettingsSection::Bar) == 1, "Bar aparece una vez") && ok;
  ok = expect(!groups.empty() && groups.front().module->key == "home", "Home es el primer módulo") && ok;

  const auto system = settings::moduleForSection(sidebar, SettingsSection::Power);
  ok = expect(system.has_value() && system->module->key == "system", "Energía está en Sistema") && ok;
  ok =
      expect(system.has_value() && std::ranges::contains(system->sections, SettingsSection::Services), "Servicios a la vista, sin Más")
      && ok;
  const auto osd = settings::moduleForSection(sidebar, SettingsSection::Notifications);
  ok = expect(
           osd.has_value() && osd->module->key == "osd" && osd->sections.front() == SettingsSection::Osd,
           "OSD agrupa notificaciones"
       )
      && ok;

  // Una sección desconocida cae en Más de Avanzado.
  const std::vector<SettingsSection> onlyNiri{SettingsSection::Niri};
  ok = expect(settings::moduleForSection(onlyNiri, SettingsSection::Niri)->module->key == "windows", "Niri en Ventanas")
      && ok;
  // Módulos sin secciones visibles no aparecen.
  ok = expect(
           std::ranges::none_of(
               settings::groupSettingsModules(onlyNiri), [](const auto& g) { return g.module->key == "extensions"; }
           ),
           "sin módulos vacíos"
       )
      && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
