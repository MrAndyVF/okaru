#pragma once

#include "launcher/launcher_provider.h"
#include "system/desktop_entry.h"

#include <cstdint>
#include <vector>

class ConfigService;
class CompositorPlatform;

class AppProvider : public LauncherProvider {
public:
  explicit AppProvider(ConfigService* config, CompositorPlatform* platform = nullptr);
  [[nodiscard]] static std::string actionResultId(std::string_view desktopEntryPath, std::string_view desktopActionId);

  [[nodiscard]] std::string_view id() const override { return "Applications"; }
  [[nodiscard]] bool trackUsage() const override { return true; }

  void initialize() override;

  [[nodiscard]] std::vector<LauncherResult> query(std::string_view text) const override;

  bool activate(const LauncherResult& result) override;

private:
  void refreshEntriesIfNeeded() const;

  ConfigService* m_config = nullptr;
  CompositorPlatform* m_platform = nullptr;
  mutable std::vector<DesktopEntry> m_entries;
  mutable std::uint64_t m_entriesVersion = 0;
};
