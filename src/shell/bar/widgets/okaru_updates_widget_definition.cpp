#include "shell/bar/widgets/okaru_updates_widget_definition.h"

const okaru::bar::WidgetDefinition<OkaruUpdatesWidget::Options>& okaruUpdatesWidgetDefinition() {
  using Options = OkaruUpdatesWidget::Options;
  using okaru::bar::field;
  static const okaru::bar::WidgetDefinition<Options> definition{
      .type = "updates",
      .fields =
          {
              field<&Options::showCount>({.key = "show_count"}),
              field<&Options::hideWhenEmpty>({
                  .key = "hide_when_empty",
                  .presentation =
                      settings::WidgetSettingPresentation{
                          .descriptionKey = "settings.widgets.settings.hide-when-empty.updates-description",
                      },
              }),
          },
      .glyph = [](const Options&) { return std::string("package"); },
  };
  return definition;
}
