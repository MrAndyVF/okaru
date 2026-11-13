#pragma once

// OKARU: actualizaciones en el lanzador (/arch): revisar, actualizar y buscar entre los paquetes pendientes.

#include "launcher/launcher_provider.h"

class UpdatesProvider : public LauncherProvider {
public:
  [[nodiscard]] std::string_view defaultPrefix() const override { return "arch"; }
  [[nodiscard]] std::string_view id() const override { return "Updates"; }
  [[nodiscard]] std::string displayName() const override;
  [[nodiscard]] std::string_view defaultGlyphName() const override { return "package"; }

  [[nodiscard]] std::vector<LauncherResult> query(std::string_view text) const override;
  bool activate(const LauncherResult& result) override;
};
