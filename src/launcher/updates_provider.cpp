#include "launcher/updates_provider.h"

#include "core/deferred_call.h"
#include "i18n/i18n.h"
#include "shell/panel/panel_manager.h"
#include "system/okaru_updates.h"
#include "util/fuzzy_match.h"

using okaru::updates::Engine;

std::string UpdatesProvider::displayName() const { return i18n::tr("okaru.updates.title"); }

std::vector<LauncherResult> UpdatesProvider::query(std::string_view text) const {
  const auto& engine = Engine::instance();
  std::vector<LauncherResult> results;
  if (text.empty()) {
    results.push_back({.id = "cmd:check", .title = i18n::tr("okaru.updates.check"),
                       .subtitle = i18n::tr("okaru.updates.launcher-check"), .glyphName = "refresh"});
    results.push_back({.id = "cmd:update", .title = i18n::tr("okaru.updates.update"),
                       .subtitle = okaru::updates::statusText(engine), .glyphName = "download"});
    results.push_back({.id = "cmd:panel", .title = i18n::tr("okaru.updates.title"),
                       .subtitle = i18n::tr("okaru.updates.launcher-panel"), .glyphName = "package"});
    return results;
  }
  for (const auto* source : {&engine.pacman(), &engine.aur(), &engine.flatpak(), &engine.snap()}) {
    for (const auto& item : source->items) {
      const double score = FuzzyMatch::score(text, item.name);
      if (!FuzzyMatch::isMatch(score)) {
        continue;
      }
      results.push_back({.id = "pkg:" + item.name, .title = item.name,
                         .subtitle = item.from.empty() ? item.to : item.from + " → " + item.to,
                         .glyphName = item.source == "flatpak" ? "app-window" : item.source == "snap" ? "box" : "package",
                         .score = score});
    }
  }
  return results;
}

bool UpdatesProvider::activate(const LauncherResult& result) {
  auto& engine = Engine::instance();
  if (result.id == "cmd:check") {
    engine.check();
  } else if (result.id == "cmd:update") {
    if (engine.phase() == okaru::updates::Phase::Ready) {
      engine.update();
    } else {
      engine.check();
    }
  } else {
    // Un paquete o el panel: se abre el panel (después de que el lanzador se cierre).
    DeferredCall::callLater([] { PanelManager::instance().openPanel("okaru-updates"); });
  }
  return true;
}
