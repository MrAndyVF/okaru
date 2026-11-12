#include "shell/bar/widgets/okaru_updates_widget.h"

#include "i18n/i18n.h"
#include "render/scene/input_area.h"
#include "system/okaru_updates.h"
#include "ui/builders.h"
#include "ui/controls/glyph.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <format>
#include <linux/input-event-codes.h>

using okaru::updates::Engine;
using okaru::updates::Phase;


OkaruUpdatesWidget::OkaruUpdatesWidget(wl_output* /*output*/, Options options) : m_options(options) {}

OkaruUpdatesWidget::~OkaruUpdatesWidget() {
  if (m_listener != 0) {
    Engine::instance().removeListener(m_listener);
  }
}

void OkaruUpdatesWidget::create() {
  const float scale = m_contentScale;
  auto area = ui::inputArea({});
  area->addChild(ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceXs * scale},
      ui::glyph({.out = &m_glyph, .glyph = "package", .glyphSize = Style::baseGlyphSize * scale}),
      ui::label({.out = &m_label, .fontSize = Style::fontSizeBody * fontScale(), .fontWeight = labelFontWeight(),
                 .fontFamily = labelFontFamily()})
  ));
  area->setOnClick([this](const InputArea::PointerData& data) {
    if (data.button == BTN_LEFT) {
      requestPanelToggle("okaru-updates");
    } else if (data.button == BTN_RIGHT) {
      Engine::instance().check();
    }
  });
  setRoot(std::move(area));
  m_listener = Engine::instance().addListener([this] { sync(); });
  sync();
}

void OkaruUpdatesWidget::sync() {
  auto* area = static_cast<InputArea*>(root());
  if (area == nullptr || m_glyph == nullptr) {
    return;
  }
  const auto& engine = Engine::instance();
  const Phase phase = engine.phase();
  const int total = engine.total();
  const bool reboot = engine.rebootRecommended();

  std::string text;
  if (phase == Phase::Running && engine.progressTotal() > 0) {
    text = std::format("{}%", std::min(100, engine.progressDone() * 100 / engine.progressTotal()));
  } else if (total > 0 && m_options.showCount) {
    text = std::to_string(total);
  }
  const std::string status = okaru::updates::statusText(engine);
  const std::string key = std::format("{}|{}|{}|{}|{}|{}", static_cast<int>(phase), text, reboot, status,
                                      okaru::updates::breakdown(engine), engine.checkedAt());
  if (key == m_lastKey) {
    return;
  }
  m_lastKey = key;

  const bool visible = !(m_options.hideWhenEmpty && total == 0 && !reboot && phase != Phase::Running
                         && phase != Phase::Error && phase != Phase::Missing);
  area->setVisible(visible);
  area->setParticipatesInLayout(visible);

  const ColorRole role = phase == Phase::Error || phase == Phase::Missing          ? ColorRole::Error
      : phase == Phase::Checking || phase == Phase::Running                        ? ColorRole::Secondary
      : reboot                                                                     ? ColorRole::Tertiary
      : total > 0                                                                  ? ColorRole::Primary
                                                                                   : ColorRole::OnSurface;
  m_glyph->setColor(role == ColorRole::OnSurface ? widgetIconColorOr(colorSpecFromRole(role)) : colorSpecFromRole(role));
  m_glyph->setGlyph(reboot && phase != Phase::Running ? "refresh-alert" : "package");
  m_label->setText(text);
  m_label->setVisible(!text.empty());
  m_label->setColor(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)));

  std::vector<TooltipRow> rows{{i18n::tr("okaru.updates.tooltip-status"), status}};
  if (const std::string detail = okaru::updates::breakdown(engine); !detail.empty()) {
    rows.push_back({i18n::tr("okaru.updates.tooltip-pending"), detail});
  }
  if (reboot) {
    rows.push_back({i18n::tr("okaru.updates.tooltip-reboot"), i18n::tr("okaru.updates.tooltip-reboot-value")});
  }
  if (!engine.checkedAt().empty()) {
    rows.push_back({i18n::tr("okaru.updates.tooltip-checked"), engine.checkedAt()});
  }
  rows.push_back({"", i18n::tr("okaru.updates.tooltip-hints")});
  area->setTooltip(std::move(rows));
  requestUpdate();
}

void OkaruUpdatesWidget::doLayout(Renderer& renderer, float /*containerWidth*/, float /*containerHeight*/) {
  auto* node = root();
  if (node == nullptr || node->children().empty()) {
    return;
  }
  auto* row = static_cast<Flex*>(node->children().front().get());
  row->layout(renderer);
  node->setSize(row->width(), row->height());
}
