#include "shell/bar/widgets/test_widget_definition.h"

const okaru::bar::WidgetDefinition<std::monostate>& testWidgetDefinition() {
  static const okaru::bar::WidgetDefinition<std::monostate> definition{
      .type = "test",
  };
  return definition;
}
