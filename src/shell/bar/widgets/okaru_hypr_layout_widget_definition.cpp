#include "shell/bar/widgets/okaru_hypr_layout_widget_definition.h"

const okaru::bar::WidgetDefinition<OkaruHyprLayoutWidget::Options>& okaruHyprLayoutWidgetDefinition() {
  using Options = OkaruHyprLayoutWidget::Options;
  using okaru::bar::field;
  static const okaru::bar::WidgetDefinition<Options> definition{
      .type = "hypr_layout",
      .fields = {field<&Options::showLabel>({.key = "show_label"})},
      .glyph = [](const Options&) { return std::string("layout-board-split"); },
  };
  return definition;
}
