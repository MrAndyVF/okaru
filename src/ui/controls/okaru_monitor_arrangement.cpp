#include "ui/controls/okaru_monitor_arrangement.h"

#include "compositors/hyprland/okaru_hypr_outputs.h"
#include "cursor-shape-v1-client-protocol.h"
#include "render/core/render_styles.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/rect_node.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <linux/input-event-codes.h>
#include <memory>

namespace {

  constexpr float kPadding = 24.0F;    // margen alrededor de los monitores, en px de UI
  constexpr float kSnapPixels = 14.0F; // distancia de imán en pantalla
  constexpr float kMonitorRadius = 8.0F;

  RoundedRectStyle rectStyle(const Color& fill, const Color& border, float borderWidth, float radius) {
    return RoundedRectStyle{
        .fill = fill,
        .border = border,
        .fillMode = FillMode::Solid,
        .radius = radius,
        .softness = 1.0F,
        .borderWidth = borderWidth,
    };
  }

} // namespace

OkaruMonitorArrangement::OkaruMonitorArrangement(std::vector<Monitor> monitors, std::string selected, float scale)
    : m_monitors(std::move(monitors)), m_selected(std::move(selected)), m_uiScale(scale), m_height(240.0F * scale) {
  m_paletteConn = paletteChanged().connect([this] { applyPalette(); });

  auto background = std::make_unique<RectNode>();
  background->setParticipatesInLayout(false);
  background->setZIndex(0);
  m_background = static_cast<RectNode*>(addChild(std::move(background)));

  for (const auto& monitor : m_monitors) {
    Visual visual;
    auto rect = std::make_unique<RectNode>();
    rect->setParticipatesInLayout(false);
    rect->setZIndex(1);
    visual.rect = static_cast<RectNode*>(addChild(std::move(rect)));

    const auto makeLabel = [&](const std::string& text, float size, FontWeight weight) {
      auto label = std::make_unique<Label>();
      label->setText(text);
      label->setFontSize(size * scale);
      label->setFontWeight(weight);
      label->setTextAlign(TextAlign::Center);
      label->setParticipatesInLayout(false);
      label->setHitTestVisible(false);
      label->setFocusable(false);
      label->setZIndex(2);
      return static_cast<Label*>(addChild(std::move(label)));
    };
    visual.name = makeLabel(monitor.name, Style::fontSizeBody, FontWeight::Bold);
    visual.detail = makeLabel(monitor.detail, Style::fontSizeCaption, FontWeight::Normal);
    m_visuals.push_back(visual);
  }

  auto area = std::make_unique<InputArea>();
  area->setParticipatesInLayout(false);
  area->setZIndex(3);
  area->setOnPress([this](const InputArea::PointerData& data) {
    if (data.button != BTN_LEFT) {
      return;
    }
    if (data.pressed) {
      const int index = hitTest(data.localX, data.localY);
      if (index < 0) {
        return;
      }
      if (m_monitors[index].name != m_selected) {
        m_selected = m_monitors[index].name;
        applyPalette();
        if (m_onSelect) {
          m_onSelect(m_selected);
        }
      }
      if (!draggable(index)) {
        return;
      }
      m_dragIndex = index;
      m_fitFrozen = true;
      const double lx = (data.localX - m_offsetX) / m_fit + m_originX;
      const double ly = (data.localY - m_offsetY) / m_fit + m_originY;
      m_grabX = lx - m_monitors[index].x;
      m_grabY = ly - m_monitors[index].y;
      m_startX = m_monitors[index].x;
      m_startY = m_monitors[index].y;
      return;
    }

    // Soltar.
    const int index = m_dragIndex;
    m_dragIndex = -1;
    m_fitFrozen = false;
    if (index < 0) {
      return;
    }
    const auto& moved = m_monitors[index];
    const int x = static_cast<int>(std::lround(moved.x));
    const int y = static_cast<int>(std::lround(moved.y));
    if ((x != static_cast<int>(std::lround(m_startX)) || y != static_cast<int>(std::lround(m_startY))) && m_onMoved) {
      m_onMoved(moved.name, x, y);
    }
    markLayoutDirty();
  });
  area->setOnMotion([this](const InputArea::PointerData& data) {
    if (m_dragIndex < 0 || m_inputArea == nullptr || !m_inputArea->pressed()) {
      return;
    }
    auto& moving = m_monitors[m_dragIndex];
    okaru::hypr::Rect rect{
        .x = (data.localX - m_offsetX) / m_fit + m_originX - m_grabX,
        .y = (data.localY - m_offsetY) / m_fit + m_originY - m_grabY,
        .width = moving.width,
        .height = moving.height,
    };
    std::vector<okaru::hypr::Rect> others;
    for (int i = 0; i < static_cast<int>(m_monitors.size()); ++i) {
      if (i != m_dragIndex && draggable(i)) {
        const auto& o = m_monitors[i];
        others.push_back({.x = o.x, .y = o.y, .width = o.width, .height = o.height});
      }
    }
    rect = okaru::hypr::snapRect(rect, others, kSnapPixels * m_uiScale / m_fit);
    moving.x = rect.x;
    moving.y = rect.y;
    markLayoutDirty();
  });
  m_inputArea = static_cast<InputArea*>(addChild(std::move(area)));
  m_inputArea->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB);

  applyPalette();
}

void OkaruMonitorArrangement::setOnSelect(std::function<void(const std::string&)> callback) {
  m_onSelect = std::move(callback);
}

void OkaruMonitorArrangement::setOnMoved(std::function<void(const std::string&, int, int)> callback) {
  m_onMoved = std::move(callback);
}

bool OkaruMonitorArrangement::draggable(int index) const {
  return !m_monitors[index].disabled && !m_monitors[index].mirrored;
}

int OkaruMonitorArrangement::hitTest(float localX, float localY) const {
  // Al revés del orden de dibujo, para que gane el de arriba.
  for (int i = static_cast<int>(m_monitors.size()) - 1; i >= 0; --i) {
    const auto& m = m_monitors[i];
    const float x = static_cast<float>((m.x - m_originX) * m_fit) + m_offsetX;
    const float y = static_cast<float>((m.y - m_originY) * m_fit) + m_offsetY;
    if (localX >= x && localX <= x + m.width * m_fit && localY >= y && localY <= y + m.height * m_fit) {
      return i;
    }
  }
  return -1;
}

void OkaruMonitorArrangement::computeFit() {
  if (m_fitFrozen || m_monitors.empty()) {
    return;
  }
  // Los monitores apagados o en espejo no ocupan lugar en el escritorio: se dibujan en fila debajo.
  double minX = std::numeric_limits<double>::max();
  double minY = std::numeric_limits<double>::max();
  double maxX = std::numeric_limits<double>::lowest();
  double maxY = std::numeric_limits<double>::lowest();
  for (const auto& m : m_monitors) {
    minX = std::min(minX, m.x);
    minY = std::min(minY, m.y);
    maxX = std::max(maxX, m.x + m.width);
    maxY = std::max(maxY, m.y + m.height);
  }
  // Holgura para poder arrastrar un monitor a cualquier lado del resto.
  const double spanW = std::max(1.0, maxX - minX);
  const double spanH = std::max(1.0, maxY - minY);
  const float pad = kPadding * m_uiScale;
  const float availW = std::max(1.0F, width() - pad * 2.0F);
  const float availH = std::max(1.0F, m_height - pad * 2.0F);
  m_fit = std::min(availW / (spanW * 1.5), availH / (spanH * 1.5));
  m_originX = minX;
  m_originY = minY;
  m_offsetX = static_cast<float>((width() - spanW * m_fit) * 0.5);
  m_offsetY = static_cast<float>((m_height - spanH * m_fit) * 0.5);
}

void OkaruMonitorArrangement::applyPalette() {
  if (m_background != nullptr) {
    m_background->setStyle(rectStyle(
        colorForRole(ColorRole::SurfaceContainerLow), colorForRole(ColorRole::OutlineVariant), Style::borderWidth,
        Style::scaledRadiusLg(m_uiScale)
    ));
  }
  for (std::size_t i = 0; i < m_visuals.size(); ++i) {
    const auto& m = m_monitors[i];
    const bool selected = m.name == m_selected;
    const float alpha = m.disabled ? 0.35F : 1.0F;
    const Color fill = selected ? colorForRole(ColorRole::Primary, alpha) : colorForRole(ColorRole::SurfaceContainerHigh, alpha);
    const Color border = selected ? colorForRole(ColorRole::Primary) : colorForRole(ColorRole::OnSurfaceVariant, alpha);
    m_visuals[i].rect->setStyle(rectStyle(fill, border, Style::borderWidth * 2.0F, kMonitorRadius * m_uiScale));
    const Color text = selected ? colorForRole(ColorRole::OnPrimary) : colorForRole(ColorRole::OnSurface, alpha);
    m_visuals[i].name->setColor(text);
    m_visuals[i].detail->setColor(text);
  }
  markPaintDirty();
}

LayoutSize OkaruMonitorArrangement::doMeasure(Renderer& /*renderer*/, const LayoutConstraints& constraints) {
  const float w = constraints.hasMaxWidth ? constraints.maxWidth : 600.0F * m_uiScale;
  return constraints.constrain(LayoutSize{.width = w, .height = m_height});
}

void OkaruMonitorArrangement::doLayout(Renderer& renderer) {
  computeFit();
  m_background->setPosition(0.0F, 0.0F);
  m_background->setFrameSize(width(), m_height);
  m_inputArea->setPosition(0.0F, 0.0F);
  m_inputArea->setFrameSize(width(), m_height);

  for (std::size_t i = 0; i < m_visuals.size(); ++i) {
    const auto& m = m_monitors[i];
    auto& v = m_visuals[i];
    const float x = static_cast<float>((m.x - m_originX) * m_fit) + m_offsetX;
    const float y = static_cast<float>((m.y - m_originY) * m_fit) + m_offsetY;
    const float w = static_cast<float>(m.width * m_fit);
    const float h = static_cast<float>(m.height * m_fit);
    // El que se arrastra se dibuja encima de los demás.
    v.rect->setZIndex(static_cast<int>(i) == m_dragIndex ? 4 : 1);
    v.rect->setPosition(x, y);
    v.rect->setFrameSize(w, h);

    v.name->setMaxWidth(std::max(0.0F, w - 8.0F * m_uiScale));
    v.detail->setMaxWidth(std::max(0.0F, w - 8.0F * m_uiScale));
    v.name->measure(renderer);
    v.detail->measure(renderer);
    const float textH = v.name->height() + v.detail->height();
    const float top = y + (h - textH) * 0.5F;
    v.name->setZIndex(static_cast<int>(i) == m_dragIndex ? 5 : 2);
    v.detail->setZIndex(static_cast<int>(i) == m_dragIndex ? 5 : 2);
    v.name->setPosition(x + (w - v.name->width()) * 0.5F, top);
    v.name->setFrameSize(v.name->width(), v.name->height());
    v.detail->setPosition(x + (w - v.detail->width()) * 0.5F, top + v.name->height());
    v.detail->setFrameSize(v.detail->width(), v.detail->height());
    v.detail->setVisible(h > textH + 4.0F);
  }
  setSize(width(), m_height);
}
