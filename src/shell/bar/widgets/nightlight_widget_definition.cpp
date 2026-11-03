#include "shell/bar/widgets/nightlight_widget_definition.h"

const okaru::bar::WidgetDefinition<std::monostate>& nightlightWidgetDefinition() {
  static const okaru::bar::WidgetDefinition<std::monostate> definition{
      .type = "nightlight",
  };
  return definition;
}
