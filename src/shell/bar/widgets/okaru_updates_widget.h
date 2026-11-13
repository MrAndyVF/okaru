#pragma once

// OKARU: widget de actualizaciones (antes okaru/arch-updater:widget): icono y número de pendientes, o el porcentaje
// mientras se actualiza. Clic: panel okaru-updates. Clic derecho: revisar ahora.

#include "shell/bar/widget.h"

#include <string>

class Glyph;
class Label;
struct wl_output;

class OkaruUpdatesWidget : public Widget {
public:
  struct Options {
    bool showCount = true;
    bool hideWhenEmpty = false;
  };

  OkaruUpdatesWidget(wl_output* output, Options options);
  ~OkaruUpdatesWidget() override;

  void create() override;

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void sync();

  Options m_options;
  Glyph* m_glyph = nullptr;
  Label* m_label = nullptr;
  int m_listener = 0;
  std::string m_lastKey;
};
