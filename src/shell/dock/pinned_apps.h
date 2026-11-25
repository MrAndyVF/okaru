#pragma once

#include "system/desktop_entry.h"

#include <string>
#include <string_view>
#include <vector>

class ConfigService;

namespace shell::dock::pinned_apps {

  [[nodiscard]] bool matchesEntry(const DesktopEntry& entry, std::string_view pinnedId);
  [[nodiscard]] bool containsEntry(const std::vector<std::string>& pinned, const DesktopEntry& entry);
  void removeEntry(std::vector<std::string>& pinned, const DesktopEntry& entry);
  [[nodiscard]] std::vector<DesktopEntry> resolveEntries(const std::vector<std::string>& pinned);
  // OKARU: fija o quita la app de la lista global del dock (Ajustes → Aplicaciones y el lanzador).
  [[nodiscard]] bool isPinned(const ConfigService& config, const DesktopEntry& entry);
  bool setPinned(ConfigService& config, const DesktopEntry& entry, bool pinned);

} // namespace shell::dock::pinned_apps
