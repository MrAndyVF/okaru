#include "shell/bar/widgets/okaru_ports_widget.h"

#include "core/timer_manager.h"
#include "i18n/i18n.h"
#include "render/scene/input_area.h"
#include "system/okaru_ports.h"
#include "system/system_monitor_service.h"
#include "ui/builders.h"
#include "ui/controls/glyph.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <format>
#include <linux/input-event-codes.h>

namespace {
  constexpr double kActiveBytesPerSecond = 1024.0;
  constexpr std::size_t kTooltipPorts = 8;
} // namespace

using okaru::ports::Scanner;

OkaruPortsWidget::OkaruPortsWidget(wl_output* /*output*/, Options options, SystemMonitorService* sysmon)
    : m_options(options), m_sysmon(sysmon), m_timer(std::make_unique<Timer>()) {}

OkaruPortsWidget::~OkaruPortsWidget() {
  if (m_listener != 0) {
    Scanner::instance().removeListener(m_listener);
  }
}

void OkaruPortsWidget::create() {
  const float scale = m_contentScale;
  const auto rate = [scale](Glyph** icon, Label** label, const char* glyph) {
    return ui::row(
        {.align = FlexAlign::Center, .gap = 2.0F * scale},
        ui::glyph({.out = icon, .glyph = glyph, .glyphSize = Style::baseGlyphSize * scale}),
        // monoespaciada y con ancho mínimo: la píldora no cambia de ancho con cada muestra
        ui::label({.out = label, .text = "0", .fontSize = Style::fontSizeCaption * scale, .fontFamily = "monospace",
                   .minWidth = 30.0F * scale})
    );
  };
  auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
  if (m_options.showSpeed) {
    row->addChild(rate(&m_rxIcon, &m_rx, "arrow-down"));
    row->addChild(rate(&m_txIcon, &m_tx, "arrow-up"));
  }
  // un enchufe solo mantiene el widget clicable cuando no se muestra nada más
  row->addChild(ui::row(
      {.out = &m_portsGroup, .align = FlexAlign::Center, .gap = Style::spaceXs * scale},
      ui::glyph({.glyph = "plug-connected", .glyphSize = Style::baseGlyphSize * scale,
                 .color = widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface))}),
      ui::label({.out = &m_count, .text = "0", .fontSize = Style::fontSizeCaption * scale, .fontFamily = "monospace",
                 .visible = m_options.showPorts})
  ));

  auto area = ui::inputArea({});
  area->addChild(std::move(row));
  area->setOnClick([this](const InputArea::PointerData& data) {
    if (data.button == BTN_LEFT) {
      requestPanelToggle("okaru-ports");
    } else if (data.button == BTN_MIDDLE) {
      Scanner::instance().rescan();
    }
  });
  setRoot(std::move(area));

  auto& scanner = Scanner::instance();
  scanner.configure(
      {.includeUdp = m_options.includeUdp, .hideSystemPorts = m_options.hideSystemPorts,
       .onlyOwnProcesses = m_options.onlyOwnProcesses},
      static_cast<int>(m_options.refreshInterval)
  );
  m_listener = scanner.addListener([this] { sync(); });
  if (m_options.showSpeed) {
    m_timer->startRepeating(std::chrono::seconds(1), [this] { sync(); });
  }
  sync();
}

void OkaruPortsWidget::sync() {
  const auto& listeners = Scanner::instance().listeners();
  double rx = 0.0;
  double tx = 0.0;
  if (m_sysmon != nullptr && m_sysmon->isRunning()) {
    const auto stats = m_sysmon->latest();
    rx = stats.netRxBytesPerSec;
    tx = stats.netTxBytesPerSec;
  }
  const auto rateColor = [](double value) {
    return colorSpecFromRole(value > kActiveBytesPerSecond ? ColorRole::Primary : ColorRole::OnSurfaceVariant);
  };
  if (m_rx != nullptr) {
    m_rx->setText(okaru::ports::compactRate(rx));
    m_tx->setText(okaru::ports::compactRate(tx));
    m_rxIcon->setColor(rateColor(rx));
    m_txIcon->setColor(rateColor(tx));
  }
  const bool portsShown = m_options.showPorts && !listeners.empty();
  if (m_portsGroup != nullptr) {
    m_portsGroup->setVisible(portsShown || !m_options.showSpeed);
  }
  if (m_count != nullptr) {
    m_count->setText(std::to_string(listeners.size()));
    m_count->setVisible(portsShown);
  }
  if (auto* area = static_cast<InputArea*>(root()); area != nullptr) {
    std::string tooltip =
        i18n::tr("okaru.ports.tooltip-speed", "rx", okaru::ports::fullRate(rx), "tx", okaru::ports::fullRate(tx));
    for (std::size_t i = 0; i < std::min(listeners.size(), kTooltipPorts); ++i) {
      const auto& l = listeners[i];
      tooltip += std::format("\n:{}  {}", l.port, l.process.empty() ? i18n::tr("okaru.ports.unknown-process") : l.process);
    }
    if (listeners.size() > kTooltipPorts) {
      tooltip += "\n" + i18n::tr("okaru.ports.tooltip-more", "count", std::to_string(listeners.size() - kTooltipPorts));
    }
    area->setTooltip(tooltip);
  }
  requestRedraw();
}

void OkaruPortsWidget::doLayout(Renderer& renderer, float /*containerWidth*/, float /*containerHeight*/) {
  auto* node = root();
  if (node == nullptr || node->children().empty()) {
    return;
  }
  auto* row = static_cast<Flex*>(node->children().front().get());
  row->layout(renderer);
  node->setSize(row->width(), row->height());
}
