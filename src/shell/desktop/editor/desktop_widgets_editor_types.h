#pragma once

#include "config/config_types.h"

#include <string_view>

using DesktopWidgetsEditorSnapshot = DesktopWidgetsConfig;

struct DesktopWidgetsEditorProfile {
  std::string_view logSection;
  std::string_view layerNamespace;
  std::string_view widgetIdPrefix;
  std::string_view titleKey;

  [[nodiscard]] static DesktopWidgetsEditorProfile desktop();
};
