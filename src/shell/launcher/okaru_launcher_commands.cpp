#include "shell/launcher/okaru_launcher_commands.h"

#include "compositors/compositor_detect.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "core/process/process.h"
#include "i18n/i18n.h"
#include "shell/panel/panel_manager.h"
#include "util/fuzzy_match.h"
#include "util/string_utils.h"

#include <array>

namespace {

  constexpr Logger kLog("launcher");

  struct Command {
    std::string_view id;
    std::string_view glyph;
    std::string_view keywords; // en inglés y español, para encontrarlo en cualquiera de los dos idiomas
  };

  constexpr std::array kCommands{
      Command{"keybinds", "keyboard", "keybindings shortcuts keys atajos teclado teclas combinaciones"},
      Command{"settings", "settings", "settings preferences configuration ajustes configuración preferencias"},
      Command{"emoji", "mood-smile", "emoji emojis gif kaomoji symbols símbolos"},
      Command{"clipboard", "clipboard", "clipboard history copy paste portapapeles historial copiar pegar"},
      Command{"session", "power", "session power shutdown reboot logout lock suspend sesión apagar reiniciar "
                                  "cerrar bloquear suspender"},
      Command{"wallpaper", "photo", "wallpaper background fondo fondos pantalla"},
  };

  // Abre un panel después de que okaru-launcher se cierre (el gestor muestra uno a la vez).
  void openPanelLater(std::string id) {
    DeferredCall::callLater([id = std::move(id)] { PanelManager::instance().openPanel(id, PanelOpenRequest{}); });
  }

} // namespace

std::vector<LauncherResult> OkaruLauncherCommands::query(std::string_view text) const {
  const std::string pattern = StringUtils::toLower(StringUtils::trim(text));
  std::vector<LauncherResult> results;
  for (const Command& command : kCommands) {
    if (command.id == "keybinds" && !compositors::isHyprland()) {
      continue; // los atajos se leen de la config Lua de Hyprland
    }
    LauncherResult result{
        .id = std::string(command.id),
        .title = i18n::tr("launcher.okaru.command." + std::string(command.id) + ".title"),
        .subtitle = i18n::tr("launcher.okaru.command." + std::string(command.id) + ".subtitle"),
        .glyphName = std::string(command.glyph),
    };
    if (!pattern.empty()) {
      const std::string haystack =
          StringUtils::toLower(result.title + ' ' + result.subtitle + ' ' + std::string(command.keywords));
      result.score = FuzzyMatch::score(pattern, haystack);
      if (!FuzzyMatch::isMatch(result.score)) {
        continue;
      }
    }
    results.push_back(std::move(result));
  }
  return results;
}

bool OkaruLauncherCommands::activate(const LauncherResult& result) {
  if (result.id == "keybinds") {
    if (m_showKeybinds) {
      m_showKeybinds();
    }
    return false; // se queda abierto, en la vista de atajos
  }
  if (result.id == "settings") {
    DeferredCall::callLater([] { PanelManager::instance().openSettingsWindow(); });
  } else if (result.id == "emoji") {
    if (!process::runAsync({"okaru-emoji"})) {
      kLog.warn("could not start okaru-emoji");
    }
  } else {
    openPanelLater(result.id); // clipboard, session, wallpaper: ids de panel
  }
  return true;
}
