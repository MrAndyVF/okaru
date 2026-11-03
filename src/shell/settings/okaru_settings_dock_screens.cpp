#include "shell/settings/okaru_settings_dock_screens.h"

#include "config/config_service.h"
#include "i18n/i18n.h"
#include "shell/settings/settings_content_common.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <memory>

namespace settings {

  namespace {

    std::vector<std::string> enabledPath(const std::string& output) { return {"dock", "monitor", output, "enabled"}; }

    std::unique_ptr<Flex> row(float scale, const std::string& title, const std::string& detail) {
      auto text = ui::column(
          {.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale, .flexGrow = 1.0F},
          makeLabel(title, Style::fontSizeBody * scale, colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold)
      );
      auto label = makeLabel(detail, Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant));
      label->setMaxLines(3);
      text->addChild(std::move(label));
      auto out = ui::row({.align = FlexAlign::Center, .gap = Style::spaceMd * scale, .fillWidth = true});
      out->addChild(std::move(text));
      return out;
    }

    // Todas: sin overrides `enabled`. Una: enabled=false en las demás y sin override en la elegida.
    void apply(ConfigService& config, const std::vector<std::string>& outputs, bool all, const std::string& screen) {
      std::vector<std::vector<std::string>> clear;
      std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> set;
      for (const auto& output : outputs) {
        if (all || output == screen) {
          clear.push_back(enabledPath(output));
        } else {
          set.emplace_back(enabledPath(output), false);
        }
      }
      bool changed = false;
      (void)config.clearOverrides(clear, &changed);
      if (!set.empty()) {
        (void)config.setOverrides(set);
      }
    }

  } // namespace

  DockScreensState readDockScreens(const ConfigService& config, const std::vector<std::string>& outputs) {
    DockScreensState state;
    const auto& overrides = config.config().dock.monitorOverrides;
    const auto disabled = [&](const std::string& output) {
      return std::ranges::any_of(overrides, [&](const DockMonitorOverride& o) {
        return o.match == output && o.enabled.has_value() && !*o.enabled;
      });
    };
    for (const auto& output : outputs) {
      if (disabled(output)) {
        state.allScreens = false;
      } else if (state.screen.empty()) {
        state.screen = output;
      }
    }
    return state;
  }

  void addDockScreensCard(Flex& content, float scale, ConfigService& config, const std::vector<std::string>& outputs) {
    if (outputs.size() < 2) {
      return; // con una sola pantalla no hay nada que elegir
    }
    const DockScreensState state = readDockScreens(config, outputs);
    Flex* body = addSettingsCard(content, i18n::tr("settings.okaru.dock.screens"), scale);

    auto allRow =
        row(scale, i18n::tr("settings.okaru.dock.all-screens"), i18n::tr("settings.okaru.dock.all-screens-desc"));
    allRow->addChild(
        ui::toggle({
            .checked = state.allScreens,
            .scale = scale,
            .onChange = [&config, outputs, screen = state.screen](bool on) {
              apply(config, outputs, on, screen.empty() ? outputs.front() : screen);
            },
        })
    );
    body->addChild(std::move(allRow));

    if (!state.allScreens) {
      auto screenRow = row(scale, i18n::tr("settings.okaru.dock.screen"), i18n::tr("settings.okaru.dock.screen-desc"));
      const auto it = std::ranges::find(outputs, state.screen);
      screenRow->addChild(
          ui::select({
              .options = outputs,
              .selectedIndex = it == outputs.end() ? 0 : static_cast<std::size_t>(it - outputs.begin()),
              .fontSize = Style::fontSizeCaption * scale,
              .controlHeight = Style::controlHeightSm * scale,
              .width = 200.0F * scale,
              .onSelectionChanged = [&config, outputs](std::size_t index, std::string_view) {
                apply(config, outputs, false, outputs[index]);
              },
          })
      );
      body->addChild(std::move(screenRow));
    }
  }

} // namespace settings
