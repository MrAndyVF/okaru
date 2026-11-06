#pragma once

#include "shell/bar/widget.h"
#include "shell/bar/widget_custom_image.h"

#include <cstdint>
#include <string>

class BluetoothService;
class Glyph;
class INetworkService;
class Image;
class PipeWireService;
struct wl_output;

class ControlCenterWidget : public Widget {
public:
  struct Options {
    std::string glyph = "settings"; // engranaje (el glifo "okaru" es el búho de Noctalia)
    std::string customImage;
    bool customImageColorize = false;
  };

  // OKARU: además del icono del centro de control, los de red, bluetooth y sonido (como en DMS).
  ControlCenterWidget(
      wl_output* output, Options options, INetworkService* network = nullptr, BluetoothService* bluetooth = nullptr,
      PipeWireService* audio = nullptr
  );

  void create() override;

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void doUpdate(Renderer& renderer) override; // OKARU
  INetworkService* m_network = nullptr;       // OKARU
  BluetoothService* m_bluetooth = nullptr;    // OKARU
  PipeWireService* m_audio = nullptr;         // OKARU
  Glyph* m_networkGlyph = nullptr;            // OKARU
  Glyph* m_bluetoothGlyph = nullptr;          // OKARU
  Glyph* m_volumeGlyph = nullptr;             // OKARU
  std::string m_barGlyphId;
  WidgetCustomImage m_customImage;
  Glyph* m_glyph = nullptr;
  Image* m_image = nullptr;
};
