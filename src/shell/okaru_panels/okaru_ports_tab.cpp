#include "shell/okaru_panels/okaru_ports_tab.h"

#include "core/deferred_call.h"
#include "core/process/process.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "shell/panel/panel_button_style.h"
#include "shell/panel/panel_manager.h"
#include "system/okaru_ports.h"
#include "system/system_monitor_service.h"
#include "ui/builders.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <format>

using namespace control_center;
using okaru::ports::Listener;
using okaru::ports::Scanner;

namespace {
  constexpr int kStopRescanDelayMs = 600;

  std::string metaLine(const Listener& l) {
    std::string out = l.proto == "tcp" ? "TCP" : "UDP";
    if (l.pid > 0) {
      out += " · " + i18n::tr("okaru.ports.pid", "pid", std::to_string(l.pid));
    }
    std::string addresses;
    for (const auto& a : l.addresses) {
      addresses += (addresses.empty() ? "" : ", ") + a;
    }
    return out + " · " + addresses;
  }
} // namespace

OkaruPortsTab::OkaruPortsTab(SystemMonitorService* sysmon) : m_sysmon(sysmon) {}

OkaruPortsTab::~OkaruPortsTab() {
  if (m_listener != 0) {
    Scanner::instance().removeListener(m_listener);
  }
}

std::unique_ptr<Flex> OkaruPortsTab::create() {
  const float scale = contentScale();
  const float opacity = panelCardOpacity();
  auto root = ui::column({.out = &m_root, .align = FlexAlign::Stretch, .gap = Style::spaceMd * scale});

  const auto legend = [scale](Label** out, const char* glyph, ColorRole role) {
    return ui::row(
        {.align = FlexAlign::Center, .gap = Style::spaceXs * scale},
        ui::glyph({.glyph = glyph, .glyphSize = Style::fontSizeBody * scale, .color = colorSpecFromRole(role)}),
        ui::label({.out = out, .text = "0 B/s", .fontSize = Style::fontSizeBody * scale, .fontFamily = "monospace",
                   .color = colorSpecFromRole(role)})
    );
  };
  auto speed = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceLg * scale,
      .configure =
          [scale, opacity](Flex& card) {
            applySectionCardStyle(card, scale, opacity);
            card.setDirection(FlexDirection::Horizontal);
            card.setAlign(FlexAlign::Center);
          },
  });
  speed->addChild(legend(&m_rx, "download-speed", ColorRole::Primary));
  speed->addChild(legend(&m_tx, "upload-speed", ColorRole::Secondary));
  root->addChild(std::move(speed));

  auto ports = ui::column({
      .flexGrow = 1.0F,
      .configure = [scale, opacity](Flex& card) { applySectionCardStyle(card, scale, opacity); },
  });
  auto header = makeCardHeaderRow(i18n::tr("okaru.ports.listening"), scale);
  header->addChild(ui::label({.out = &m_count, .text = "0", .fontSize = Style::fontSizeBody * scale,
                              .fontFamily = "monospace"}));
  ports->addChild(std::move(header));
  auto scroll = ui::scrollView({
      .contentScale = scale,
      .scrollbarVisible = true,
      .viewportPaddingH = 0.0F,
      .viewportPaddingV = 0.0F,
      .flexGrow = 1.0F,
      .configure = [](ScrollView& view) {
        view.clearFill();
        view.clearBorder();
      },
  });
  m_rows = scroll->content();
  m_rows->setDirection(FlexDirection::Vertical);
  m_rows->setAlign(FlexAlign::Stretch);
  m_rows->setGap(Style::spaceXs * scale);
  ports->addChild(std::move(scroll));
  root->addChild(std::move(ports));

  m_rowsKey = "-";
  m_listener = Scanner::instance().addListener([] { PanelManager::instance().refresh(); });
  m_timer.startRepeating(std::chrono::seconds(1), [] { PanelManager::instance().refresh(); });
  return root;
}

std::unique_ptr<Flex> OkaruPortsTab::createHeaderActions() {
  const float scale = contentScale();
  return ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceSm * scale},
      ui::button({
          .glyph = "refresh",
          .tooltip = i18n::tr("okaru.ports.refresh"),
          .onClick = [] { Scanner::instance().rescan(); },
          .configure = [scale](Button& button) { panel_button_style::configureHeaderIconButton(button, scale); },
      })
  );
}

void OkaruPortsTab::setActive(bool active) {
  if (active) {
    Scanner::instance().rescan();
  }
}

void OkaruPortsTab::onClose() {
  if (m_listener != 0) {
    Scanner::instance().removeListener(m_listener);
    m_listener = 0;
  }
  m_timer.stop();
  m_root = nullptr;
  m_rows = nullptr;
  m_rx = nullptr;
  m_tx = nullptr;
  m_count = nullptr;
}

void OkaruPortsTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_root == nullptr) {
    return;
  }
  rebuildRows();
  m_root->setSize(contentWidth, bodyHeight);
  m_root->layout(renderer);
}

void OkaruPortsTab::doUpdate(Renderer& /*renderer*/) {
  if (m_rx != nullptr && m_sysmon != nullptr && m_sysmon->isRunning()) {
    const auto stats = m_sysmon->latest();
    m_rx->setText(okaru::ports::fullRate(stats.netRxBytesPerSec));
    m_tx->setText(okaru::ports::fullRate(stats.netTxBytesPerSec));
  }
  rebuildRows();
}

void OkaruPortsTab::rebuildRows() {
  if (m_rows == nullptr) {
    return;
  }
  const auto& scanner = Scanner::instance();
  std::string key = scanner.failed() ? "!" : "";
  for (const auto& l : scanner.listeners()) {
    key += l.key + ';';
  }
  if (key == m_rowsKey) {
    return;
  }
  m_rowsKey = key;
  const float scale = contentScale();
  while (!m_rows->children().empty()) {
    m_rows->removeChild(m_rows->children().back().get());
  }
  m_count->setText(std::to_string(scanner.listeners().size()));

  if (scanner.listeners().empty()) {
    m_rows->addChild(ui::column(
        {.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .paddingV = Style::spaceLg * scale},
        ui::glyph({.glyph = "plug-connected-x", .glyphSize = 40.0F * scale,
                   .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}),
        ui::label({.text = i18n::tr(scanner.failed() ? "okaru.ports.scan-error" : "okaru.ports.none"),
                   .fontSize = Style::fontSizeBody * scale, .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)})
    ));
    return;
  }

  for (const Listener& l : scanner.listeners()) {
    const ColorRole chip = l.localOnly ? ColorRole::OnSurfaceVariant : ColorRole::Tertiary;
    auto row = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceMd * scale,
        .paddingV = Style::spaceSm * scale,
        .paddingH = Style::spaceMd * scale,
        .fill = colorSpecFromRole(ColorRole::Surface, 0.5F),
        .radius = Style::radiusMd * scale,
    });
    row->addChild(ui::label({.text = std::format("{:<5}", l.port), .fontSize = Style::fontSizeTitle * scale,
                             .fontWeight = FontWeight::Bold, .fontFamily = "monospace",
                             .color = colorSpecFromRole(ColorRole::Primary)}));
    row->addChild(ui::column(
        {.align = FlexAlign::Start, .gap = 2.0F * scale, .flexGrow = 1.0F},
        ui::label({.text = l.process.empty() ? i18n::tr("okaru.ports.unknown-process") : l.process,
                   .fontSize = Style::fontSizeBody * scale,
                   .color = colorSpecFromRole(l.process.empty() ? ColorRole::OnSurfaceVariant : ColorRole::OnSurface),
                   .maxLines = 1}),
        ui::label({.text = metaLine(l), .fontSize = Style::fontSizeCaption * scale,
                   .color = colorSpecFromRole(ColorRole::OnSurfaceVariant), .maxLines = 1})
    ));
    row->addChild(ui::row(
        {.paddingV = 2.0F * scale, .paddingH = Style::spaceSm * scale, .fill = colorSpecFromRole(chip, 0.12F),
         .radius = Style::radiusSm * scale},
        ui::label({.text = i18n::tr(l.localOnly ? "okaru.ports.local" : "okaru.ports.exposed"),
                   .fontSize = Style::fontSizeCaption * scale, .color = colorSpecFromRole(chip)})
    ));
    if (l.proto == "tcp") {
      row->addChild(ui::button({
          .glyph = "external-link",
          .glyphSize = Style::fontSizeBody * scale,
          .variant = ButtonVariant::Ghost,
          .tooltip = i18n::tr("okaru.ports.open"),
          .onClick =
              [port = l.port] {
                (void)process::runAsync({"xdg-open", std::format("http://localhost:{}", port)});
                PanelManager::instance().closePanel();
              },
      }));
    }
    row->addChild(ui::button({
        .glyph = "x",
        .glyphSize = Style::fontSizeBody * scale,
        .variant = ButtonVariant::Ghost,
        .tooltip = i18n::tr("okaru.ports.stop"),
        .onClick =
            [l] {
              process::RunCallbacks callbacks;
              callbacks.onExit = [](process::RunResult) {
                DeferredCall::callLater([] { Scanner::instance().rescan(kStopRescanDelayMs); });
              };
              (void)process::runAsync(okaru::ports::stopCommand(l), std::move(callbacks));
            },
    }));
    m_rows->addChild(std::move(row));
  }
}
