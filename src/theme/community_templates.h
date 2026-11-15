#pragma once

#include "config/config_types.h"
#include "theme/builtin_templates.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class HttpClient;

namespace okaru::theme {

  class CommunityTemplateService {
  public:
    using ReadyCallback = std::function<void()>;

    explicit CommunityTemplateService(HttpClient& httpClient);

    void setReadyCallback(ReadyCallback callback);
    // OKARU: true (una vez) si desde la última consulta se descargó algún archivo de una plantilla activa.
    [[nodiscard]] bool takeSelectedFilesUpdated() { return std::exchange(m_selectedFilesUpdated, false); }
    void sync(const ThemeConfig::TemplatesConfig& templates);

    [[nodiscard]] static std::vector<AvailableTemplate> availableTemplates();

  private:
    void syncSelectedFromCatalog(
        const std::vector<std::string>& selectedIds, std::uint64_t generation, bool notifyWhenReady
    );

    void syncCatalogManifests(const std::vector<std::string>& selectedIds, std::uint64_t generation);
    HttpClient& m_httpClient;
    ReadyCallback m_readyCallback;
    std::uint64_t m_generation = 0;
    bool m_selectedFilesUpdated = false;
  };

  [[nodiscard]] std::filesystem::path communityTemplatesCacheDir();
  [[nodiscard]] std::filesystem::path communityTemplateDir(std::string_view id);
  [[nodiscard]] std::filesystem::path communityTemplateConfigPath(std::string_view id);
  [[nodiscard]] bool isSafeCommunityTemplateId(std::string_view id);

} // namespace okaru::theme
