#include "ui/controls/card_columns.h"

#include <algorithm>
#include <array>

CardColumns::CardColumns(float minColumnWidth, float gap) : m_minColumnWidth(minColumnWidth), m_gap(gap) {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
}

int CardColumns::columnsFor(float width, float minColumnWidth, float gap) noexcept {
  return width >= minColumnWidth * 2.0F + gap ? 2 : 1;
}

float CardColumns::place(Renderer& renderer, float width, bool arrange) {
  const int columns = columnsFor(width, m_minColumnWidth, m_gap);
  const float columnWidth = columns == 2 ? (width - m_gap) * 0.5F : width;
  std::array<float, 2> heights{0.0F, 0.0F};
  std::array<bool, 2> used{false, false};
  int index = 0;
  for (const auto& child : children()) {
    if (!child->visible() || !child->participatesInLayout()) {
      continue;
    }
    const int column = index++ % columns;
    LayoutConstraints constraints;
    constraints.setExactWidth(columnWidth);
    const LayoutSize size = child->measure(renderer, constraints);
    const float y = heights[column] + (used[column] ? m_gap : 0.0F);
    if (arrange) {
      child->arrange(
          renderer,
          LayoutRect{.x = column * (columnWidth + m_gap), .y = y, .width = columnWidth, .height = size.height}
      );
    }
    heights[column] = y + size.height;
    used[column] = true;
  }
  return std::max(heights[0], heights[1]);
}

void CardColumns::doLayout(Renderer& renderer) { place(renderer, width(), true); }

LayoutSize CardColumns::doMeasure(Renderer& renderer, const LayoutConstraints& constraints) {
  const float w = constraints.hasMaxWidth ? constraints.maxWidth : width();
  return constraints.constrain(LayoutSize{.width = w, .height = place(renderer, w, false)});
}

void CardColumns::doArrange(Renderer& renderer, const LayoutRect& rect) {
  setPosition(rect.x, rect.y);
  const float h = place(renderer, rect.width, true);
  setSize(rect.width, std::max(rect.height, h));
}
