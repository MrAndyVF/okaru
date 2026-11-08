#include "shell/desktop/desktop_widget_layout.h"
#include "tests/test_check.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace {

  WaylandOutput makeOutput(std::string connector, std::string model, std::string serial, int width, int height) {
    static char outputToken;
    WaylandOutput output;
    output.connectorName = std::move(connector);
    output.description = "Acme " + model;
    output.make = "Acme";
    output.model = std::move(model);
    output.serialNumber = std::move(serial);
    // Selection only reads output metadata; the proxy is never dereferenced.
    output.output = reinterpret_cast<wl_output*>(&outputToken);
    output.done = true;
    output.logicalWidth = width;
    output.logicalHeight = height;
    return output;
  }

  DesktopWidgetState makeWidget(std::string id, std::string selector, float width, float height) {
    DesktopWidgetState widget;
    widget.id = std::move(id);
    widget.outputName = std::move(selector);
    widget.cx = width * 0.25F;
    widget.cy = height * 0.75F;
    widget.placementWidth = width;
    widget.placementHeight = height;
    return widget;
  }

  void checkSelection() {
    std::vector<WaylandOutput> outputs{
        makeOutput("DP-3", "Landscape", "LANDSCAPE123", 2560, 1440),
        makeOutput("DP-4", "Portrait", "PORTRAIT456", 1296, 2304),
    };
    TEST_CHECK(desktop_widgets::selectOutput(outputs, "DP-3").output == outputs.data());
    TEST_CHECK(desktop_widgets::selectOutput(outputs, "Acme Landscape").output == outputs.data());
    TEST_CHECK(desktop_widgets::selectOutput(outputs, "Portrait").output == &outputs[1]);
    TEST_CHECK(desktop_widgets::selectOutput(outputs, "PORTRAIT456").output == &outputs[1]);
    TEST_CHECK(desktop_widgets::selectOutput(outputs, "PORTRAIT").output == nullptr);
    TEST_CHECK(desktop_widgets::selectOutput(outputs, "portrait").output == nullptr);
    TEST_CHECK(desktop_widgets::selectOutput(outputs, "Acme").ambiguous);
    TEST_CHECK(desktop_widgets::selectOutput(outputs, "Acme").output == nullptr);
    TEST_CHECK(!desktop_widgets::selectOutput(outputs, "missing").ambiguous);
    TEST_CHECK(desktop_widgets::selectOutput(outputs, "missing").output == nullptr);
    TEST_CHECK(desktop_widgets::selectOutput(outputs, "").output == outputs.data());

    // An exact connector assignment wins even if another description contains it.
    outputs[0].description = "Acme DP-4";
    TEST_CHECK(desktop_widgets::selectOutput(outputs, "DP-4").output == &outputs[1]);
    std::ranges::reverse(outputs);
    TEST_CHECK(desktop_widgets::selectOutput(outputs, "DP-4").output == outputs.data());

    outputs[0].done = false;
    TEST_CHECK(!desktop_widgets::selectOutput(outputs, "Acme").ambiguous);
    TEST_CHECK(desktop_widgets::selectOutput(outputs, "Acme").output == &outputs[1]);
    outputs[0].done = true;
    outputs[0].output = nullptr;
    TEST_CHECK(desktop_widgets::selectOutput(outputs, "Portrait").output == nullptr);
    outputs[0] = makeOutput("DP-4", "Portrait", "PORTRAIT456", 0, 0);
    TEST_CHECK(desktop_widgets::selectOutput(outputs, "Portrait").output == nullptr);
  }

  void checkPlacementAcrossDocking() {
    std::vector<WaylandOutput> outputs{
        makeOutput("DP-3", "Landscape", "LANDSCAPE123", 2560, 1440),
        makeOutput("DP-4", "Portrait", "PORTRAIT456", 1296, 2304),
    };
    std::vector<DesktopWidgetState> widgets{
        makeWidget("landscape-clock", "Landscape", 2560.0F, 1440.0F),
        makeWidget("portrait-clock", "PORTRAIT456", 1296.0F, 2304.0F),
    };
    const auto original = widgets;
    const desktop_widgets::PlacementMapper mapper;
    TEST_CHECK(!mapper.remapForOutputChange(outputs, widgets));

    // Connectors can be reused by the other physical monitor after docking.
    std::swap(outputs[0].connectorName, outputs[1].connectorName);
    std::ranges::reverse(outputs);
    TEST_CHECK(!mapper.remapForOutputChange(outputs, widgets));
    mapper.rebaseForCurrentOutputs(outputs, widgets);
    TEST_CHECK(widgets == original);

    outputs.clear();
    TEST_CHECK(!mapper.remapForOutputChange(outputs, widgets));
    mapper.rebaseForCurrentOutputs(outputs, widgets);
    TEST_CHECK(widgets == original);

    outputs.push_back(makeOutput("DP-11", "Landscape", "LANDSCAPE123", 1280, 720));
    TEST_CHECK(mapper.remapForOutputChange(outputs, widgets));
    TEST_CHECK(widgets[0].cx == original[0].cx * 0.5F);
    TEST_CHECK(widgets[0].cy == original[0].cy * 0.5F);
    TEST_CHECK(widgets[0].outputName == "Landscape");
    TEST_CHECK(widgets[1] == original[1]);
    TEST_CHECK(!mapper.remapForOutputChange(outputs, widgets));

    // Ambiguity must never choose a monitor and persist its geometry.
    outputs.push_back(makeOutput("DP-12", "Landscape", "ANOTHER789", 3840, 2160));
    const auto beforeAmbiguity = widgets;
    TEST_CHECK(!mapper.remapForOutputChange(outputs, widgets));
    mapper.rebaseForCurrentOutputs(outputs, widgets);
    TEST_CHECK(widgets == beforeAmbiguity);
  }

} // namespace

int main() {
  checkSelection();
  checkPlacementAcrossDocking();
  return 0;
}
