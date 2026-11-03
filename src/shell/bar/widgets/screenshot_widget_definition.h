#pragma once

#include "shell/bar/widget_definition.h"
#include "shell/bar/widgets/screenshot_widget.h"

[[nodiscard]] const okaru::bar::WidgetDefinition<ScreenshotWidget::Options>& screenshotWidgetDefinition();
