#pragma once

// OKARU: widget del layout de Hyprland del workspace visible en el monitor de la barra.
// Clic izquierdo: siguiente layout; derecho: anterior (okaru msg hypr-layout).

#include "compositors/hyprland/hyprland_runtime.h"
#include "shell/bar/widget.h"

#include <string>

class Glyph;
class Label;

class OkaruHyprLayoutWidget : public Widget {
public:
  struct Options {
    bool showLabel = true;
  };

  OkaruHyprLayoutWidget(std::string outputName, Options options);

  void create() override;

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void doUpdate(Renderer& renderer) override;
  void syncState();

  compositors::hyprland::HyprlandRuntime m_runtime;
  std::string m_outputName;
  Options m_options;
  Glyph* m_glyph = nullptr;
  Label* m_label = nullptr;
  std::string m_lastLayout;
};
