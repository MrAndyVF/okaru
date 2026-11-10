#pragma once

// OKARU: fila de pestañas de las apps (okaru-edit, okaru-pdf): todas del mismo ancho (hasta 240 px), nombre centrado,
// X en su recuadro a la derecha que se oscurece al pasar el puntero.

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class Node;
class Renderer;

namespace okaru::apps {

  struct TabInfo {
    std::string title;
    std::string tooltip;
  };

  struct TabStripProps {
    std::vector<TabInfo> tabs;
    std::size_t active = 0;
    std::string closeTooltip;
    std::function<void(std::size_t)> onActivate;
    std::function<void(std::size_t)> onClose;
  };

  [[nodiscard]] std::unique_ptr<Node> tabStrip(Renderer* renderer, float width, float scale, TabStripProps props);

} // namespace okaru::apps
