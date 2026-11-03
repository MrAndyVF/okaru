#include "shell/bar/widgets/theme_mode_widget_definition.h"

const okaru::bar::WidgetDefinition<std::monostate>& themeModeWidgetDefinition() {
  static const okaru::bar::WidgetDefinition<std::monostate> definition{
      .type = "theme_mode",
  };
  return definition;
}
