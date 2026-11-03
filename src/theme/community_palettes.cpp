#include "theme/community_palettes.h"

#include "core/deferred_call.h"
#include "core/log.h"
#include "net/http_client.h"
#include "util/checksum.h"
#include "util/file_utils.h"
#include "util/string_utils.h"

#include <algorithm>
#include <array>
#include <exception>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace okaru::theme {

  namespace {

    constexpr Logger kLog("community_palettes");
    // OKARU: catálogo propio (github.com/MrAndyVF/okaru-palettes); cada paleta en palettes/<nombre>.json.
    constexpr std::string_view kCatalogUrl = "https://raw.githubusercontent.com/MrAndyVF/okaru-palettes/main/catalog.json";
    constexpr std::string_view kPaletteUrlBase = "https://raw.githubusercontent.com/MrAndyVF/okaru-palettes/main/palettes";

    std::filesystem::path catalogCachePath() { return communityPaletteCacheDir() / ".catalog" / "palettes.json"; }
    // OKARU: paletas propias (forks locales de las ya descargadas), con prioridad sobre el catálogo remoto.
    std::filesystem::path localCatalogPath() { return communityPaletteCacheDir() / ".catalog" / "local-palettes.json"; }

    std::string stringField(const nlohmann::json& obj, std::string_view key) {
      auto it = obj.find(std::string(key));
      if (it == obj.end() || !it->is_string()) {
        return {};
      }
      return it->get<std::string>();
    }

    std::string paletteNameFromJson(const nlohmann::json& item) {
      if (item.is_string()) {
        return item.get<std::string>();
      }
      if (!item.is_object()) {
        return {};
      }
      return stringField(item, "name");
    }

    AvailablePalette::PreviewMode palettePreviewFromJson(const nlohmann::json& item, std::string_view mode) {
      if (!item.is_object()) {
        return {};
      }
      auto modeIt = item.find(std::string(mode));
      if (modeIt == item.end() || !modeIt->is_object()) {
        return {};
      }

      static constexpr auto kPreviewFields = std::to_array<std::string_view>({
          "primary",
          "secondary",
          "tertiary",
          "error",
      });

      AvailablePalette::PreviewMode preview;
      preview.accents.reserve(kPreviewFields.size());
      preview.surface = stringField(*modeIt, "surface");
      for (const auto field : kPreviewFields) {
        std::string color = stringField(*modeIt, field);
        if (!color.empty()) {
          preview.accents.push_back(std::move(color));
        }
      }
      return preview;
    }

    std::vector<AvailablePalette> parseCatalogFile(const std::filesystem::path& path) {
      std::ifstream in(path);
      if (!in) {
        return {};
      }

      try {
        std::stringstream buf;
        buf << in.rdbuf();
        const auto root = nlohmann::json::parse(buf.str());
        const nlohmann::json* entries = &root;
        if (root.is_object()) {
          if (auto it = root.find("palettes"); it != root.end()) {
            entries = &*it;
          }
        }
        if (!entries->is_array()) {
          return {};
        }

        std::vector<AvailablePalette> out;
        out.reserve(entries->size());
        for (const auto& item : *entries) {
          AvailablePalette palette;
          palette.name = paletteNameFromJson(item);
          palette.md5 = item.is_object() ? stringField(item, "md5") : std::string{};
          palette.preview.dark = palettePreviewFromJson(item, "dark");
          palette.preview.light = palettePreviewFromJson(item, "light");
          if (!palette.name.empty()) {
            out.push_back(std::move(palette));
          }
        }
        std::ranges::sort(out, {}, &AvailablePalette::name);
        out.erase(std::ranges::unique(out, {}, &AvailablePalette::name).begin(), out.end());
        return out;
      } catch (const std::exception& e) {
        kLog.warn("failed to parse community palette catalog {}: {}", path.string(), e.what());
        return {};
      }
    }

    std::vector<AvailablePalette> loadCatalog() {
      auto remote = parseCatalogFile(catalogCachePath());
      auto local = parseCatalogFile(localCatalogPath());
      std::erase_if(remote, [&local](const AvailablePalette& p) {
        return std::ranges::contains(local, p.name, &AvailablePalette::name);
      });
      local.insert(local.end(), std::make_move_iterator(remote.begin()), std::make_move_iterator(remote.end()));
      std::ranges::sort(local, {}, &AvailablePalette::name);
      return local;
    }

    // OKARU: una sola vez, las paletas descargadas pasan a ser propias (md5 del archivo local).
    void ensureLocalCatalog() {
      std::error_code ec;
      if (std::filesystem::exists(localCatalogPath(), ec) || !std::filesystem::exists(catalogCachePath(), ec)) {
        return;
      }
      nlohmann::json out = nlohmann::json::array();
      try {
        std::ifstream in(catalogCachePath());
        const auto root = nlohmann::json::parse(in);
        const nlohmann::json& items = root.is_object() && root.contains("palettes") ? root["palettes"] : root;
        for (const auto& item : items) {
          const std::string name = paletteNameFromJson(item);
          const auto path = communityPaletteCachePath(name);
          if (name.empty() || !std::filesystem::is_regular_file(path, ec)) {
            continue;
          }
          nlohmann::json copy = item;
          copy["md5"] = util::fileMd5Hex(path);
          out.push_back(std::move(copy));
        }
      } catch (const std::exception& e) {
        kLog.warn("could not build local palette catalog: {}", e.what());
        return;
      }
      std::ofstream(localCatalogPath()) << out.dump(1);
    }

  } // namespace

  CommunityPaletteService::CommunityPaletteService(HttpClient& httpClient) : m_httpClient(httpClient) {}

  void CommunityPaletteService::setReadyCallback(ReadyCallback callback) { m_readyCallback = std::move(callback); }

  void CommunityPaletteService::sync() {
    const std::uint64_t generation = ++m_generation;
    const std::filesystem::path path = catalogCachePath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    ensureLocalCatalog(); // OKARU: antes de que el catálogo propio reemplace al viejo

    m_httpClient.download(kCatalogUrl, path, [this, generation, path](bool success) {
      if (generation != m_generation) {
        return;
      }
      if (!success) {
        kLog.warn("failed to refresh community palette catalog; using cached metadata when available");
        return;
      }
      if (parseCatalogFile(path).empty()) {
        kLog.warn("community palette catalog downloaded but contained no palettes");
      }
      if (m_readyCallback) {
        DeferredCall::callLater([callback = m_readyCallback] { callback(); });
      }
    });
  }

  std::vector<AvailablePalette> availableCommunityPalettes() { return loadCatalog(); }

  std::string communityPaletteCatalogMd5(std::string_view name) {
    const auto catalog = loadCatalog();
    auto it = std::ranges::find(catalog, name, &AvailablePalette::name);
    if (it == catalog.end()) {
      return {};
    }
    return it->md5;
  }

  // Stored under the state dir (not XDG cache): app-managed, re-fetchable data that
  // should persist between sessions and not be auto-reclaimed by cache cleaners.
  std::filesystem::path communityPaletteCacheDir() {
    const std::string state = FileUtils::stateDir();
    if (!state.empty()) {
      return std::filesystem::path(state) / "community-palettes";
    }
    return std::filesystem::path("/tmp") / "okaru" / "community-palettes";
  }

  std::filesystem::path communityPaletteCachePath(std::string_view name) {
    return communityPaletteCacheDir() / (StringUtils::urlEncode(name) + ".json");
  }

  std::string communityPaletteDownloadUrl(std::string_view name) {
    return std::string(kPaletteUrlBase) + "/" + StringUtils::urlEncode(name) + ".json";
  }

} // namespace okaru::theme
