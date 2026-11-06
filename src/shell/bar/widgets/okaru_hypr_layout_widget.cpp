#include "shell/bar/widgets/okaru_hypr_layout_widget.h"

#include "compositors/hyprland/okaru_hypr_layouts.h"
#include "i18n/i18n.h"
#include "render/scene/input_area.h"
#include "ui/builders.h"
#include "ui/controls/glyph.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>

OkaruHyprLayoutWidget::OkaruHyprLayoutWidget(std::string outputName, Options options)
    : m_outputName(std::move(outputName)), m_options(options) {}

void OkaruHyprLayoutWidget::create() {
  auto area = ui::inputArea({});
  area->addChild(ui::glyph({.out = &m_glyph, .glyph = "layout-grid", .glyphSize = Style::baseGlyphSize * m_contentScale}));
  if (m_options.showLabel) {
    area->addChild(ui::label({.out = &m_label, .fontSize = Style::fontSizeBody * fontScale(),
                              .fontWeight = labelFontWeight(), .fontFamily = labelFontFamily()}));
  }
  setRoot(std::move(area));
}

void OkaruHyprLayoutWidget::doLayout(Renderer& renderer, float /*containerWidth*/, float /*containerHeight*/) {
  auto* rootNode = root();
  if (rootNode == nullptr || m_glyph == nullptr) {
    return;
  }
  syncState();
  m_glyph->measure(renderer);
  float height = m_glyph->height();
  float x = m_glyph->width();
  if (m_label != nullptr) {
    m_label->measure(renderer);
    height = std::max(height, m_label->height());
    m_label->setPosition(x + Style::spaceXs, std::round((height - m_label->height()) * 0.5F));
    x += Style::spaceXs + m_label->width();
  }
  m_glyph->setPosition(0.0F, std::round((height - m_glyph->height()) * 0.5F));
  rootNode->setSize(x, height);
}

void OkaruHyprLayoutWidget::doUpdate(Renderer& /*renderer*/) { syncState(); }

// La barra pide actualizar al cambiar de workspace, al usar hypr-layout y una vez por minuto.
void OkaruHyprLayoutWidget::syncState() {
  if (m_glyph == nullptr) {
    return;
  }
  const std::string layout = okaru::hypr::outputLayout(m_runtime, m_outputName);
  if (layout == m_lastLayout) {
    return;
  }
  m_lastLayout = layout;
  const std::string name = layout.empty() ? std::string{"—"} : i18n::tr("okaru.hypr-layout." + layout);
  m_glyph->setGlyph(std::string(okaru::hypr::layoutGlyph(layout)));
  m_glyph->setColor(widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)));
  if (m_label != nullptr) {
    m_label->setText(name);
    m_label->setColor(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)));
  }
  if (auto* area = static_cast<InputArea*>(root()); area != nullptr) {
    area->setTooltip(std::vector<TooltipRow>{{name, i18n::tr("okaru.hypr-layout.hint")}});
  }
  requestUpdate(); // el ancho cambia con el nombre
}
