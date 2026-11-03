#include "shell/bar/widgets/text_widget_definition.h"

const okaru::bar::WidgetDefinition<TextWidget::Options>& textWidgetDefinition() {
  using okaru::bar::field;
  using Options = TextWidget::Options;

  static const okaru::bar::WidgetDefinition<Options> definition{
      .type = "text",
      .fields = {
          field<&Options::text>({
              .key = "text",
          }),
      },
  };
  return definition;
}
