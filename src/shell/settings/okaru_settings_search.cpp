#include "shell/settings/okaru_settings_search.h"

#include "i18n/i18n.h"
#include "shell/settings/okaru_settings_categories.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace settings {

  namespace {

    std::string lower(std::string_view text) {
      std::string out(text);
      std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return out;
    }

    // (sección, prefijo de traducción, claves de etiqueta). La clave también es palabra clave en inglés.
    struct PageKeys {
      const char* section;
      const char* prefix;
      std::vector<const char*> keys;
    };

    const std::vector<PageKeys>& pages() {
      static const std::vector<PageKeys> k{
          {"displays",
           "settings.okaru.displays.",
           {"arrangement", "primary", "resolution", "refresh", "scale", "rotation", "mirror", "bitdepth", "cm",
            "reserved", "enabled", "sdr-brightness", "sdr-saturation"}},
          {"hypr-workspaces", "settings.okaru.workspaces.", {"title", "icon", "name", "persistent", "default", "add"}},
          {"system-startup", "settings.okaru.startup.", {"apps", "add-app", "add-command", "hyprland-start"}},
          {"system-environment", "settings.okaru.startup.", {"env", "env-add"}},
          {"hypr-plugins", "settings.okaru.hyprpm.", {"title", "installed", "add", "update", "suggestions"}},
          {"hypr-diagnostics", "settings.okaru.diagnostics.", {"status", "reload", "backups", "restore", "blocks"}},
          {"sound-devices", "settings.okaru.sound.", {"outputs", "inputs", "rename"}},
          {"home",
           "settings.okaru.home.",
           {"shell-version", "compositor", "os", "kernel", "cpu", "memory", "disks", "uptime"}},
          {"dock", "settings.okaru.dock.", {"screens", "all-screens", "screen"}},
          {"window-rules", "settings.okaru.rules.", {"effects", "category-apps", "category-shell"}},
          {"input-binds", "settings.okaru.binds.", {"title", "new", "move-all"}},
      };
      return k;
    }

  } // namespace

  bool okaruSearchMatches(const OkaruSearchItem& item, std::string_view query) {
    const std::string haystack = lower(item.label + " " + item.detail + " " + item.keywords);
    std::istringstream words{lower(query)};
    std::string word;
    bool any = false;
    while (words >> word) {
      any = true;
      if (haystack.find(word) == std::string::npos) {
        return false;
      }
    }
    return any;
  }

  std::vector<OkaruSearchItem> okaruSearchIndex() {
    std::vector<OkaruSearchItem> items;
    for (const auto& page : pages()) {
      for (const char* key : page.keys) {
        const std::string labelKey = std::string(page.prefix) + key;
        std::string keywords = key;
        std::ranges::replace(keywords, '-', ' ');
        items.push_back({
            .section = page.section,
            .label = i18n::tr(labelKey),
            .detail = i18n::Service::instance().lookup(labelKey + "-desc").empty() ? std::string()
                                                                                   : i18n::tr(labelKey + "-desc"),
            .keywords = keywords,
        });
      }
    }
    auto windows = okaruWindowsSearchItems();
    items.insert(items.end(), windows.begin(), windows.end());
    return items;
  }

  std::vector<OkaruSearchItem> okaruSearch(std::string_view query) {
    std::vector<OkaruSearchItem> out;
    for (auto& item : okaruSearchIndex()) {
      if (!sectionHiddenForCompositor(item.section) && okaruSearchMatches(item, query)) {
        out.push_back(std::move(item));
      }
    }
    return out;
  }

} // namespace settings
