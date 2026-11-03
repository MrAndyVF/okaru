#pragma once

#include "shell/bar/widget_definition.h"
#include "shell/bar/widgets/okaru_capture_widget.h"

[[nodiscard]] const okaru::bar::WidgetDefinition<OkaruCaptureWidget::Options>& okaruCaptureWidgetDefinition();
