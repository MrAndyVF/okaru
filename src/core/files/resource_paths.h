#pragma once

#include <filesystem>
#include <string_view>

namespace paths {

  [[nodiscard]] const std::filesystem::path& assetsRoot();
  [[nodiscard]] std::filesystem::path assetPath(std::string_view relativePath);
  // OKARU: datos propios de una app (traducciones…): <recursos>/../apps/<app>, o sea /usr/share/okaru/apps/<app>
  // instalado y <repo>/apps/<app> en desarrollo.
  [[nodiscard]] std::filesystem::path appDataDir(std::string_view app);

} // namespace paths
