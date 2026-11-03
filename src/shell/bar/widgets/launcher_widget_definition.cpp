#include "shell/bar/widgets/launcher_widget_definition.h"

#include "shell/bar/widgets/glyph_button_definition.h"

const okaru::bar::WidgetDefinition<LauncherWidget::Options>& launcherWidgetDefinition() {
  using Options = LauncherWidget::Options;

  static const okaru::bar::WidgetDefinition<Options> definition{
      .type = "launcher",
      .fields = okaru::bar::glyphButtonFields<Options>(),
      .glyph = [](const Options& options) { return options.glyph; },
  };
  return definition;
}
