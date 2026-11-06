#include "config/okaru_bar_fixed.h"

#include "compositors/compositor_detect.h"
#include "config/config_types.h"

#include <algorithm>
#include <array>
#include <string_view>

namespace okaru::bar {

  namespace {

    constexpr std::string_view kWorkspaces = "workspaces";
    bool g_enabled = true;

    // Aspecto propio de cada widget: en OKARU lo define la barra, no el widget.
    constexpr std::array<std::string_view, 14> kWidgetLookKeys{
        "capsule",
        "capsule_radius",
        "capsule_fill",
        "capsule_border",
        "capsule_border_width",
        "capsule_foreground",
        "capsule_padding",
        "capsule_opacity",
        "color",
        "icon_color",
        "scale",
        "font_scale",
        "font_family",
        "font_weight",
    };

    void fixLook(BarConfig& bar) {
      bar.name = "default";
      bar.position = "top";
      bar.enabled = true;
      bar.layer = "top";
      bar.thickness = 25;
      bar.border = colorSpecFromRole(ColorRole::OutlineVariant);
      bar.borderWidth = 0.0F;
      bar.radius = bar.radiusTopLeft = bar.radiusTopRight = bar.radiusBottomLeft = bar.radiusBottomRight = 0;
      bar.concaveEdgeCorners = true;
      bar.marginEnds = 0;
      bar.marginEdge = 0;
      bar.marginOppositeEdge = 0;
      bar.padding = 4;
      bar.widgetSpacing = 6;
      bar.shadow = true;
      bar.contactShadow = false;
      bar.capsuleThickness = 0.76F;
      bar.scale = 0.8F;
      bar.fontScale = 1.0F;
      bar.fontWeight = 500;
      bar.fontFamily.reset();
      bar.widgetCapsuleDefault = true;
      bar.widgetCapsuleFill = colorSpecFromRole(ColorRole::SurfaceContainerHighest);
      bar.widgetCapsuleForeground.reset();
      bar.widgetColor.reset();
      bar.widgetIconColor.reset();
      bar.widgetCapsulePadding = 6.0F;
      bar.widgetCapsuleRadius = 0.0;
      bar.widgetCapsuleOpacity = 0.0F;
      bar.widgetCapsuleBorderSpecified = true;
      bar.widgetCapsuleBorder = colorSpecFromRole(ColorRole::OutlineVariant);
      bar.widgetCapsuleBorderWidth = 0.0F;
      bar.hoverHighlight = true;
      bar.monitorOverrides.clear();

      // Grupos: se conservan miembros y acordeón; el aspecto es el de las cápsulas de la barra.
      for (auto& group : bar.widgetCapsuleGroups) {
        group.fill = bar.widgetCapsuleFill;
        group.borderSpecified = true;
        group.border = bar.widgetCapsuleBorder;
        group.borderWidth = 0.0F;
        group.foreground.reset();
        group.padding = 6.0F;
        group.radius = 0.0F;
        group.opacity = 0.0F;
        group.widgetSpacing = 10;
      }
    }

    void workspacesFirst(BarConfig& bar) {
      const auto drop = [](std::vector<std::string>& list) { std::erase(list, std::string(kWorkspaces)); };
      drop(bar.startWidgets);
      drop(bar.centerWidgets);
      drop(bar.endWidgets);
      for (auto& group : bar.widgetCapsuleGroups) {
        drop(group.members);
      }
      bar.startWidgets.insert(bar.startWidgets.begin(), std::string(kWorkspaces));
    }

  } // namespace

  void setFixedBarEnabled(bool enabled) { g_enabled = enabled; }

  void applyFixedBar(Config& config) {
    if (!g_enabled) {
      return;
    }
    if (config.bars.empty()) {
      config.bars.emplace_back();
    }
    // Una sola barra: la "default" si existe, si no la primera.
    const auto it = std::ranges::find(config.bars, std::string("default"), &BarConfig::name);
    BarConfig bar = it != config.bars.end() ? *it : config.bars.front();
    config.bars.clear();
    fixLook(bar);
    workspacesFirst(bar);
    config.bars.push_back(std::move(bar));

    // Dock en un solo apartado: de los overrides por monitor solo queda si se muestra o no.
    for (auto& ovr : config.dock.monitorOverrides) {
      ovr = DockMonitorOverride{.tableName = ovr.tableName, .match = ovr.match, .enabled = ovr.enabled};
    }

    for (auto& [name, widget] : config.widgets) {
      for (const auto key : kWidgetLookKeys) {
        widget.settings.erase(std::string(key));
      }
    }
    // Recuadros de workspaces más pequeños que los de upstream.
    auto& workspaces = config.widgets[std::string(kWorkspaces)];
    if (workspaces.type.empty()) {
      workspaces.type = std::string(kWorkspaces);
    }
    workspaces.settings.insert_or_assign("pill_scale", 1.0);
    workspaces.settings.insert_or_assign("active_pill_size", 1.75);
    workspaces.settings.insert_or_assign("inactive_pill_size", 1.75);
    workspaces.settings.insert_or_assign("capsule_radius", 0.0);

    // Esquinas rectas en toda la shell: no es una opción.
    config.shell.cornerRadiusScale = 0.0F;
  }

} // namespace okaru::bar

std::vector<std::string> defaultBarEndWidgets() {
  std::vector<std::string> widgets{"media",  "tray",       "notifications", "clipboard",      "network", "bluetooth",
                                   "volume", "brightness", "battery",       "control-center", "session"};
  if (compositors::isHyprland()) {
    widgets.insert(widgets.begin() + 2, "hypr_layout"); // tras tray, como en la barra del autor
  }
  return widgets;
}
