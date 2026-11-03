#include "shell/bar/widgets/caffeine_widget_definition.h"

const okaru::bar::WidgetDefinition<std::monostate>& caffeineWidgetDefinition() {
  static const okaru::bar::WidgetDefinition<std::monostate> definition{
      .type = "caffeine",
  };
  return definition;
}
