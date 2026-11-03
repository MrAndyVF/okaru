#pragma once

#include "shell/bar/widget_definition.h"
#include "shell/bar/widgets/privacy_widget.h"

[[nodiscard]] const okaru::bar::WidgetDefinition<PrivacyWidget::Options>& privacyWidgetDefinition();
