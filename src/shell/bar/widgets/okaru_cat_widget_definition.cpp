#include "shell/bar/widgets/okaru_cat_widget_definition.h"

const okaru::bar::WidgetDefinition<OkaruCatWidget::Options>& okaruCatWidgetDefinition() {
  using Options = OkaruCatWidget::Options;
  using okaru::bar::field;
  static const okaru::bar::WidgetDefinition<Options> definition{
      .type = "cat",
      .fields =
          {
              field<&Options::catSize>({.key = "cat_size", .minValue = 12, .maxValue = 48}),
              field<&Options::showCpuPercent>({.key = "show_cpu_percent"}),
              field<&Options::walkThreshold>({.key = "walk_threshold", .minValue = 0, .maxValue = 100}),
              field<&Options::runThreshold>({.key = "run_threshold", .minValue = 0, .maxValue = 100}),
          },
      .glyph = [](const Options&) { return std::string("paw"); },
  };
  return definition;
}
