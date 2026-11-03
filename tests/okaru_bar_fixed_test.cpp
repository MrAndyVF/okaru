// OKARU: barra fija (una sola barra, aspecto fijo, workspaces primero, sin aspecto por widget).
#include "config/config_types.h"
#include "config/okaru_bar_fixed.h"

#include <cstdlib>
#include <print>

namespace {
  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "okaru_bar_fixed_test: {}", message);
    }
    return condition;
  }
} // namespace

int main() {
  bool ok = true;
  Config config;
  BarConfig main;
  main.name = "default";
  main.position = "left";
  main.thickness = 60;
  main.autoHide = true;
  main.backgroundOpacity = 0.5F;
  main.startWidgets = {"network", "cpu"};
  main.endWidgets = {"tray", "workspaces"};
  main.widgetCapsuleGroups.push_back(
      {.id = "g1", .members = {"keyboard_layout", "workspaces"}, .padding = 12.0F, .accordion = true}
  );
  main.monitorOverrides.emplace_back();
  BarConfig extra;
  extra.name = "second";
  config.bars = {extra, main};
  config.widgets["clock"].settings = {
      {"format", std::string("%H:%M")}, {"color", std::string("primary")}, {"scale", 2.0}
  };

  okaru::bar::applyFixedBar(config);
  ok = expect(config.bars.size() == 1 && config.bars[0].name == "default", "una sola barra, la default") && ok;
  const auto& bar = config.bars[0];
  ok = expect(bar.position == "top" && bar.thickness == 25 && bar.radius == 0, "aspecto fijo") && ok;
  ok = expect(bar.autoHide && bar.backgroundOpacity == 0.5F, "conserva la experiencia") && ok;
  ok = expect(bar.monitorOverrides.empty(), "sin overrides por monitor") && ok;
  ok = expect(bar.startWidgets == std::vector<std::string>{"workspaces", "network", "cpu"}, "workspaces primero") && ok;
  ok = expect(bar.endWidgets == std::vector<std::string>{"tray"}, "workspaces sale de fin") && ok;
  ok = expect(bar.widgetCapsuleGroups[0].members == std::vector<std::string>{"keyboard_layout"}, "ni de los grupos")
      && ok;
  ok = expect(
           bar.widgetCapsuleGroups[0].accordion && bar.widgetCapsuleGroups[0].padding == 6.0F,
           "grupo: acordeón sí, aspecto fijo"
       )
      && ok;
  const auto& clock = config.widgets["clock"].settings;
  ok = expect(
           clock.contains("format") && !clock.contains("color") && !clock.contains("scale"), "widget sin aspecto propio"
       )
      && ok;
  ok = expect(std::get<double>(config.widgets["workspaces"].settings.at("active_pill_size")) == 1.75, "workspaces más chicos")
      && ok;

  Config empty;
  okaru::bar::applyFixedBar(empty);
  ok = expect(empty.bars.size() == 1 && empty.bars[0].startWidgets.front() == "workspaces", "config vacía") && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
