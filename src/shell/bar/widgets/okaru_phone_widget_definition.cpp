#include "shell/bar/widgets/okaru_phone_widget_definition.h"

const okaru::bar::WidgetDefinition<OkaruPhoneWidget::Options>& okaruPhoneWidgetDefinition() {
  using Options = OkaruPhoneWidget::Options;
  using okaru::bar::field;
  static const okaru::bar::WidgetDefinition<Options> definition{
      .type = "phone",
      .fields =
          {
              field<&Options::showLabel>({.key = "show_label"}),
              field<&Options::showBattery>({.key = "show_battery"}),
          },
      .glyph = [](const Options&) { return std::string("device-mobile"); },
  };
  return definition;
}
