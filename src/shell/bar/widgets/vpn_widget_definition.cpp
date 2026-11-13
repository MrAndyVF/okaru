#include "shell/bar/widgets/vpn_widget_definition.h"

const okaru::bar::WidgetDefinition<VpnWidget::Options>& vpnWidgetDefinition() {
  using Options = VpnWidget::Options;
  using okaru::bar::field;
  static const okaru::bar::WidgetDefinition<Options> definition{
      .type = "vpn",
      .fields = {field<&Options::showName>({.key = "show_name"})},
      .glyph = [](const Options&) { return std::string("shield-lock"); },
  };
  return definition;
}
