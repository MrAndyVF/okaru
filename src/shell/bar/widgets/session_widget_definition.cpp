#include "shell/bar/widgets/session_widget_definition.h"

#include "shell/bar/widgets/glyph_button_definition.h"

const okaru::bar::WidgetDefinition<SessionWidget::Options>& sessionWidgetDefinition() {
  using Options = SessionWidget::Options;

  static const okaru::bar::WidgetDefinition<Options> definition{
      .type = "session",
      .fields = okaru::bar::glyphButtonFields<Options>(),
      .glyph = [](const Options& options) { return options.glyph; },
  };
  return definition;
}
