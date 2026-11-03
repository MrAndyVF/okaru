#pragma once

// OKARU: vista previa de la disposición de monitores con arrastrar y soltar.
// Las coordenadas son lógicas (las de Hyprland); el control las escala para que quepan.

#include "render/scene/node.h"
#include "ui/signal.h"

#include <functional>
#include <string>
#include <vector>

class InputArea;
class Label;
class RectNode;

class OkaruMonitorArrangement : public Node {
public:
  struct Monitor {
    std::string name;
    std::string detail; // p. ej. "1920×1080 · 144 Hz"
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    bool disabled = false;
    bool mirrored = false;
  };

  OkaruMonitorArrangement(std::vector<Monitor> monitors, std::string selected, float scale);

  // Al hacer clic en un monitor.
  void setOnSelect(std::function<void(const std::string& name)> callback);
  // Al soltar un monitor que se movió: nueva posición lógica, ya pegada a los bordes cercanos.
  void setOnMoved(std::function<void(const std::string& name, int x, int y)> callback);

protected:
  LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;
  void doLayout(Renderer& renderer) override;

private:
  struct Visual {
    RectNode* rect = nullptr;
    Label* name = nullptr;
    Label* detail = nullptr;
  };

  void computeFit();
  void applyPalette();
  [[nodiscard]] int hitTest(float localX, float localY) const;
  [[nodiscard]] bool draggable(int index) const;

  std::vector<Monitor> m_monitors;
  std::vector<Visual> m_visuals;
  std::string m_selected;
  float m_uiScale = 1.0F;
  float m_height = 240.0F;

  // Transformación lógica → local: local = (logical - origin) * fit + offset.
  double m_fit = 0.1;
  double m_originX = 0.0;
  double m_originY = 0.0;
  float m_offsetX = 0.0F;
  float m_offsetY = 0.0F;
  bool m_fitFrozen = false; // durante el arrastre la vista no se reajusta

  int m_dragIndex = -1;
  double m_grabX = 0.0;
  double m_grabY = 0.0;
  double m_startX = 0.0;
  double m_startY = 0.0;

  RectNode* m_background = nullptr;
  InputArea* m_inputArea = nullptr;
  std::function<void(const std::string&)> m_onSelect;
  std::function<void(const std::string&, int, int)> m_onMoved;
  Signal<>::ScopedConnection m_paletteConn;
};
