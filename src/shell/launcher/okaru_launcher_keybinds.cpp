#include "shell/launcher/okaru_launcher_keybinds.h"

#include "i18n/i18n.h"
#include "util/string_utils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <regex>
#include <utility>

namespace okaru::launcher {

  namespace {

    struct CatalogEntry {
      std::string_view group;
      std::string_view id;
      std::string_view pattern; // regex que se busca en la acción del atajo (expresión Lua)
    };

    // Orden de la vista. Los patrones cubren los atajos de fábrica y las variantes comunes del usuario.
    constexpr std::array kCatalog{
        // Un terminal que abre btop/htop es el monitor, no el terminal.
        CatalogEntry{"apps", "terminal", R"(exec_cmd\(\s*"(uwsm app -- )?(kitty|alacritty|foot|ghostty|wezterm)\b(?![^"]*\b(btop|htop)\b))"},
        CatalogEntry{"apps", "browser", R"(exec_cmd\(\s*"(uwsm app -- )?(google-chrome|chromium|firefox|zen|brave|librewolf|vivaldi))"},
        CatalogEntry{"apps", "files", R"(okaru-files)"},
        CatalogEntry{"apps", "editor", R"(exec_cmd\(\s*"(uwsm app -- )?(zeditor|zed|code|codium|okaru-edit)\b)"},
        CatalogEntry{"apps", "monitor", R"(\b(btop|htop)\b)"},
        CatalogEntry{"apps", "fastfetch", R"(fastfetch)"},
        CatalogEntry{"apps", "color-picker", R"(hyprpicker)"},
        CatalogEntry{"okaru", "launcher", R"(panel-toggle launcher"\s*\))"},
        CatalogEntry{"okaru", "keybinds", R"(:keybinds)"},
        CatalogEntry{"okaru", "emoji", R"(okaru-emoji|/emo )"},
        CatalogEntry{"okaru", "clipboard", R"(panel-toggle clipboard)"},
        CatalogEntry{"okaru", "settings", R"(settings-toggle)"},
        CatalogEntry{"okaru", "control-center", R"(panel-toggle control-center)"},
        CatalogEntry{"okaru", "notifications", R"(panel-toggle okaru-notifications)"},
        CatalogEntry{"okaru", "media", R"(panel-toggle okaru-media)"},
        CatalogEntry{"okaru", "wallpaper", R"(panel-toggle wallpaper)"},
        CatalogEntry{"okaru", "session", R"(panel-toggle session)"},
        CatalogEntry{"okaru", "nightlight", R"(nightlight)"},
        CatalogEntry{"okaru", "caffeine", R"(caffeine)"},
        CatalogEntry{"okaru", "switcher", R"(window-switcher)"},
        CatalogEntry{"windows", "close", R"(window\.close\(\))"},
        CatalogEntry{"windows", "maximize", R"(mode\s*=\s*"maximized")"},
        CatalogEntry{"windows", "fullscreen", R"(mode\s*=\s*"fullscreen")"},
        CatalogEntry{"windows", "float", R"(window\.float\()"},
        CatalogEntry{"windows", "layout", R"(hypr-layout next)"},
        CatalogEntry{"windows", "scratchpad", R"(toggle_special\(\))"},
        CatalogEntry{"windows", "drag", R"(window\.drag\(\))"},
        CatalogEntry{"windows", "resize", R"(window\.resize\(\))"},
        CatalogEntry{"screenshots", "region", R"(screenshot-region)"},
        CatalogEntry{"screenshots", "fullscreen-shot", R"(screenshot-fullscreen)"},
        CatalogEntry{"screenshots", "window", R"(screenshot-window)"},
        CatalogEntry{"session", "lock", R"(session lock)"},
        CatalogEntry{"session", "logout", R"(dsp\.exit\(\))"},
    };

    std::string userDescription(const okaru::hypr::Bind& bind) {
      const auto it = bind.options.find("description");
      return it != bind.options.end() && it->is_string() ? it->get<std::string>() : std::string{};
    }

  } // namespace

  std::vector<std::string> keyNames(std::string_view keys) {
    static const std::vector<std::pair<std::string_view, std::string_view>> kNames{
        {"super", "Super"},   {"shift", "Shift"},     {"ctrl", "Ctrl"},          {"control", "Ctrl"},
        {"alt", "Alt"},       {"return", "↵"},        {"enter", "↵"},            {"space", "@space"},
        {"escape", "Esc"},    {"tab", "Tab"},         {"left", "←"},             {"right", "→"},
        {"up", "↑"},          {"down", "↓"},          {"period", "."},           {"comma", ","},
        {"slash", "/"},       {"minus", "-"},         {"equal", "="},            {"plus", "+"},
        {"backspace", "⌫"},  {"delete", "@delete"},  {"print", "@print"},       {"mouse:272", "@mouse-left"},
        {"mouse:273", "@mouse-right"},
    };
    std::vector<std::string> out;
    std::string part;
    const auto flush = [&] {
      const std::string trimmed = StringUtils::trim(part);
      part.clear();
      if (trimmed.empty()) {
        return;
      }
      const std::string lower = StringUtils::toLower(trimmed);
      const auto it = std::ranges::find(kNames, lower, &std::pair<std::string_view, std::string_view>::first);
      if (it != kNames.end()) {
        // Las que tienen nombre en palabras se traducen (launcher.okaru.keys.*); el resto son símbolos.
        out.push_back(
            it->second.starts_with('@') ? i18n::tr("launcher.okaru.keys." + std::string(it->second.substr(1)))
                                        : std::string(it->second)
        );
      } else if (trimmed.size() == 1) {
        out.emplace_back(1, static_cast<char>(std::toupper(static_cast<unsigned char>(trimmed[0]))));
      } else {
        out.push_back(trimmed);
      }
    };
    for (const char c : keys) {
      if (c == '+') {
        flush();
      } else {
        part.push_back(c);
      }
    }
    flush();
    return out;
  }

  std::vector<KeybindItem> curatedKeybinds(const std::vector<okaru::hypr::Bind>& binds) {
    std::vector<KeybindItem> items;
    std::vector<bool> used(binds.size(), false);
    for (const CatalogEntry& entry : kCatalog) {
      const std::regex pattern{std::string(entry.pattern)};
      const okaru::hypr::Bind* best = nullptr;
      for (std::size_t i = 0; i < binds.size(); ++i) {
        if (std::regex_search(binds[i].dispatcher, pattern)) {
          used[i] = true;
          // Varios atajos para lo mismo (btop): el más corto, que es el que se recuerda.
          if (best == nullptr || binds[i].keys.size() < best->keys.size()) {
            best = &binds[i];
          }
        }
      }
      if (best != nullptr) {
        items.push_back({std::string(entry.group), std::string(entry.id), keyNames(best->keys)});
      }
    }
    // Los propios del usuario, solo si les puso descripción (las de fábrica ya salieron arriba).
    for (std::size_t i = 0; i < binds.size(); ++i) {
      if (const std::string description = userDescription(binds[i]); !used[i] && !description.empty()) {
        items.push_back({"custom", description, keyNames(binds[i].keys)});
      }
    }
    return items;
  }

} // namespace okaru::launcher
