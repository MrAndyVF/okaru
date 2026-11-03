#include "shell/bar/widgets/okaru_ports_widget_definition.h"

const okaru::bar::WidgetDefinition<OkaruPortsWidget::Options>& okaruPortsWidgetDefinition() {
  using Options = OkaruPortsWidget::Options;
  using okaru::bar::field;
  static const okaru::bar::WidgetDefinition<Options> definition{
      .type = "ports",
      .fields =
          {
              field<&Options::showSpeed>({.key = "show_speed"}),
              field<&Options::showPorts>({.key = "show_ports"}),
              field<&Options::refreshInterval>({.key = "refresh_interval", .minValue = 1, .maxValue = 30}),
              field<&Options::hideSystemPorts>({.key = "hide_system_ports"}),
              field<&Options::onlyOwnProcesses>({.key = "only_own_processes"}),
              field<&Options::includeUdp>({.key = "include_udp"}),
          },
      .glyph = [](const Options&) { return std::string("plug-connected"); },
  };
  return definition;
}
