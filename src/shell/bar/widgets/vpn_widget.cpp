#include "shell/bar/widgets/vpn_widget.h"

#include "config/config_service.h"
#include "dbus/network/inetwork_service.h"
#include "dbus/network/vpn_profiles.h"
#include "i18n/i18n.h"
#include "render/scene/input_area.h"
#include "ui/builders.h"
#include "ui/controls/glyph.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <linux/input-event-codes.h>

VpnWidget::VpnWidget(INetworkService* network, ConfigService* config, wl_output* /*output*/, Options options)
    : m_network(network), m_config(config), m_options(options) {}

void VpnWidget::create() {
  const float scale = m_contentScale;
  auto area = ui::inputArea({});
  area->addChild(
      ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceXs * scale},
          ui::glyph({.out = &m_glyph, .glyph = "shield-lock", .glyphSize = Style::baseGlyphSize * scale}),
          ui::label(
              {.out = &m_label,
               .fontSize = Style::fontSizeBody * fontScale(),
               .fontWeight = labelFontWeight(),
               .fontFamily = labelFontFamily()}
          )
      )
  );
  area->setOnClick([this](const InputArea::PointerData& data) {
    if (data.button == BTN_LEFT) {
      requestPanelToggle("okaru-vpn");
    } else if (data.button == BTN_RIGHT) {
      toggleDefault();
    }
  });
  setRoot(std::move(area));
  sync();
}

void VpnWidget::toggleDefault() {
  if (m_network == nullptr) {
    return;
  }
  const std::string preferred = m_config != nullptr ? m_config->config().vpn.defaultConnection : std::string{};
  if (!vpn::toggleDefault(*m_network, preferred)) {
    requestPanelToggle("okaru-vpn"); // varias y ninguna por defecto: que elija en la pestaña
  }
}

void VpnWidget::doUpdate(Renderer& /*renderer*/) { sync(); }

void VpnWidget::sync() {
  auto* area = static_cast<InputArea*>(root());
  if (area == nullptr || m_glyph == nullptr) {
    return;
  }
  std::string active;
  if (m_network != nullptr) {
    for (const auto& v : m_network->vpnConnections()) {
      if (v.active) {
        active += (active.empty() ? "" : ", ") + v.name;
      }
    }
  }
  const bool connected = m_network != nullptr && m_network->state().vpnConnected;
  const std::string key = active + (connected ? "|1" : "|0");
  if (key == m_lastKey) {
    return;
  }
  m_lastKey = key;
  // Sin VPN: escudo tachado; conectando: candado; conectada: escudo con check en el color primario.
  m_glyph->setGlyph(active.empty() ? "shield-off" : connected ? "shield-check" : "shield-lock");
  m_glyph->setColor(
      active.empty() ? widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface))
                     : colorSpecFromRole(connected ? ColorRole::Primary : ColorRole::Secondary)
  );
  m_label->setText(m_options.showName ? active : std::string{});
  m_label->setVisible(m_options.showName && !active.empty());
  m_label->setColor(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)));
  area->setTooltip(
      std::vector<TooltipRow>{
          {i18n::tr("okaru.vpn.title"), active.empty() ? i18n::tr("okaru.vpn.widget-off") : active},
          {"", i18n::tr("okaru.vpn.widget-hints")},
      }
  );
  requestUpdate();
}

void VpnWidget::doLayout(Renderer& renderer, float /*containerWidth*/, float /*containerHeight*/) {
  auto* node = root();
  if (node == nullptr || node->children().empty()) {
    return;
  }
  auto* row = static_cast<Flex*>(node->children().front().get());
  row->layout(renderer);
  node->setSize(row->width(), row->height());
}
