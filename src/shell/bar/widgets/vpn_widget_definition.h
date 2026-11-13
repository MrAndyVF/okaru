#pragma once

#include "shell/bar/widget_definition.h"
#include "shell/bar/widgets/vpn_widget.h"

[[nodiscard]] const okaru::bar::WidgetDefinition<VpnWidget::Options>& vpnWidgetDefinition();
