#pragma once

// OKARU: widget del teléfono (KDE Connect nativo): icono del dispositivo, nombre opcional y batería.

#include "shell/bar/widget.h"

#include <string>

class Glyph;
class KdeConnectService;
class Label;
struct wl_output;

class OkaruPhoneWidget : public Widget {
public:
  struct Options {
    bool showLabel = false;
    bool showBattery = true;
  };

  OkaruPhoneWidget(KdeConnectService* service, wl_output* output, Options options);

  void create() override;

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void doUpdate(Renderer& renderer) override;
  void syncState(Renderer& renderer);

  KdeConnectService* m_service = nullptr;
  Options m_options;
  Glyph* m_glyph = nullptr;
  Label* m_name = nullptr;
  Glyph* m_batteryGlyph = nullptr;
  Label* m_battery = nullptr;
  std::string m_lastKey;
};
