#include "shell/settings/okaru_settings_categories.h"

#include "shell/settings/settings_registry.h"

#include <algorithm>
#include <unordered_map>

namespace settings {
  namespace {

    using S = SettingsSection;

    const std::vector<SettingsModuleDef>& modules() {
      static const std::vector<SettingsModuleDef> kModules{
          {"home", "home", 0, {S::Home}},
          {"appearance", "palette", 1, {S::Appearance, S::Wallpaper, S::Templates}},
          {"displays", "device-desktop", 1, {S::Displays, S::HyprWorkspaces}},
          {"sound", "volume", 1, {S::SoundDevices, S::Sound}},
          {"bar", "crop-3-2", 2, {S::Bar}},
          {"dock", "layout-bottombar", 2, {S::Dock}},
          {"desktop", "layout-dashboard", 2, {S::Desktop}},
          {"osd", "message", 2, {S::Osd, S::Notifications}},
          {"shell",
           "app-window",
           3,
           {S::Shell, S::Launcher, S::Screenshot, S::Calendar, S::Panels}},
          {"windows",
           "layout-columns",
           3,
           {S::WindowLayout, S::WindowDecoration, S::WindowAnimations, S::WindowRules, S::Niri}},
          {"input", "keyboard", 3, {S::InputDevices, S::InputBinds, S::Keybinds}},
          {"system",
           "settings",
           4,
           {S::SystemStartup, S::SystemEnvironment, S::Power, S::Security, S::Location, S::Services, S::System}},
          {"extensions", "puzzle", 4, {S::Plugins, S::HyprPlugins}},
          {"advanced", "stethoscope", 4, {S::HyprDiagnostics, S::Hooks}},
      };
      return kModules;
    }

    bool assigned(SettingsSection section) {
      return std::ranges::any_of(modules(), [section](const SettingsModuleDef& m) {
        return std::ranges::contains(m.sections, section);
      });
    }

  } // namespace

  bool SettingsModuleGroup::contains(SettingsSection section) const {
    return std::ranges::contains(sections, section);
  }

  std::span<const SettingsModuleDef> settingsModules() { return modules(); }

  std::string settingsModuleLabelKey(const SettingsModuleDef& module) {
    return "settings.navigation.modules." + std::string(module.key);
  }

  std::string settingsModuleHintKey(const SettingsModuleDef& module) {
    return "settings.navigation.modules." + std::string(module.key) + "-hint";
  }

  std::vector<SettingsModuleGroup> groupSettingsModules(std::span<const SettingsSection> sections) {
    const auto visible = [&](SettingsSection section) {
      return section == S::Bar || std::ranges::contains(sections, section);
    };
    std::vector<SettingsModuleGroup> groups;
    for (const auto& module : modules()) {
      SettingsModuleGroup group{.module = &module, .sections = {}};
      for (const auto section : module.sections) {
        if (visible(section)) {
          group.sections.push_back(section);
        }
      }
      // Las secciones sin módulo propio (p. ej. nuevas de upstream) se muestran en Avanzado.
      if (module.key == "advanced") {
        for (const auto section : sections) {
          if (!assigned(section)) {
            group.sections.push_back(section);
          }
        }
      }
      if (group.size() > 0) {
        groups.push_back(std::move(group));
      }
    }
    return groups;
  }

  std::optional<SettingsModuleGroup>
  moduleForSection(std::span<const SettingsSection> sections, SettingsSection section) {
    for (auto& group : groupSettingsModules(sections)) {
      if (group.contains(section)) {
        return group;
      }
    }
    return std::nullopt;
  }

  namespace {
    std::unordered_map<std::string, std::string>& lastSections() {
      static std::unordered_map<std::string, std::string> s;
      return s;
    }
  } // namespace

  std::string lastModuleSection(std::string_view moduleKey) {
    const auto it = lastSections().find(std::string(moduleKey));
    return it == lastSections().end() ? std::string() : it->second;
  }

  void rememberModuleSection(std::string_view moduleKey, std::string sectionId) {
    lastSections()[std::string(moduleKey)] = std::move(sectionId);
  }

} // namespace settings
