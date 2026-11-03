#include "shell/bar/widgets/screenshot_widget_definition.h"

#include "shell/bar/widgets/glyph_button_definition.h"

const okaru::bar::WidgetDefinition<ScreenshotWidget::Options>& screenshotWidgetDefinition() {
  using Options = ScreenshotWidget::Options;

  static const okaru::bar::WidgetDefinition<Options> definition{
      .type = "screenshot",
      .fields = okaru::bar::glyphButtonFields<Options>(),
  };
  return definition;
}
