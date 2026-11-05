#include "shell/bar/widgets/okaru_phone_widget.h"

#include "dbus/kdeconnect/kdeconnect_service.h"
#include "i18n/i18n.h"
#include "render/scene/input_area.h"
#include "shell/okaru_phone/okaru_phone_prefs.h"
#include "ui/builders.h"
#include "ui/controls/glyph.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace phone = okaru::phone;

namespace {
  constexpr int kLowBattery = 20;
}

OkaruPhoneWidget::OkaruPhoneWidget(KdeConnectService* service, wl_output* /*output*/, Options options)
    : m_service(service), m_options(options) {}

void OkaruPhoneWidget::create() {
  auto area = ui::inputArea({});
  const float glyphSize = Style::baseGlyphSize * m_contentScale;
  area->addChild(ui::glyph({.out = &m_glyph, .glyph = "device-mobile-off", .glyphSize = glyphSize}));
  if (m_options.showLabel) {
    area->addChild(ui::label({.out = &m_name, .fontSize = Style::fontSizeBody * fontScale(),
                              .fontWeight = labelFontWeight(), .fontFamily = labelFontFamily()}));
  }
  if (m_options.showBattery) {
    area->addChild(ui::glyph({.out = &m_batteryGlyph, .glyph = "battery", .glyphSize = glyphSize * 0.85F}));
    area->addChild(ui::label({.out = &m_battery, .fontSize = Style::fontSizeBody * fontScale(),
                              .fontWeight = labelFontWeight(), .fontFamily = labelFontFamily()}));
  }
  setRoot(std::move(area));
}

void OkaruPhoneWidget::doLayout(Renderer& renderer, float /*containerWidth*/, float /*containerHeight*/) {
  auto* rootNode = root();
  if (rootNode == nullptr || m_glyph == nullptr) {
    return;
  }
  syncState(renderer);

  // Los elementos se colocan en fila, centrados en vertical; los ocultos no ocupan espacio.
  m_glyph->measure(renderer);
  if (m_name != nullptr) {
    m_name->measure(renderer);
  }
  if (m_batteryGlyph != nullptr) {
    m_batteryGlyph->measure(renderer);
    m_battery->measure(renderer);
  }
  Node* parts[] = {m_glyph, m_name, m_batteryGlyph, m_battery};
  float height = 0.0F;
  for (Node* part : parts) {
    if (part != nullptr && part->visible()) {
      height = std::max(height, part->height());
    }
  }
  float x = 0.0F;
  for (Node* part : parts) {
    if (part == nullptr || !part->visible() || part->width() <= 0.0F) {
      continue;
    }
    if (x > 0.0F) {
      x += part == m_battery ? Style::spaceXs * 0.5F : Style::spaceXs;
    }
    part->setPosition(x, std::round((height - part->height()) * 0.5F));
    x += part->width();
  }
  rootNode->setSize(x, height);
}

void OkaruPhoneWidget::doUpdate(Renderer& renderer) { syncState(renderer); }

void OkaruPhoneWidget::syncState(Renderer& renderer) {
  if (m_glyph == nullptr) {
    return;
  }
  const PhoneDevice* d =
      m_service != nullptr && m_service->available() ? phone::selectedDevice(*m_service) : nullptr;
  const bool usable = d != nullptr && phone::usable(*d);
  const bool hasBattery = usable && d->battery >= 0;
  const std::string name = d != nullptr ? phone::displayName(*d) : std::string{};

  const std::string key = d == nullptr
      ? std::string{"-"}
      : std::format("{}|{}|{}|{}|{}|{}|{}", d->id, name, usable, d->battery, d->charging, d->type,
                    d->pairRequestedByPeer);
  if (key == m_lastKey) {
    return;
  }
  m_lastKey = key;

  // Un pedido de emparejamiento resalta el icono: hay que aceptarlo o rechazarlo en el panel.
  const ColorRole iconRole = d != nullptr && d->pairRequestedByPeer ? ColorRole::Primary
      : usable                                                       ? ColorRole::OnSurface
                                                                     : ColorRole::OnSurfaceVariant;
  m_glyph->setGlyph(d != nullptr ? phone::deviceGlyph(*d) : "device-mobile-off");
  m_glyph->setColor(usable ? widgetIconColorOr(colorSpecFromRole(iconRole)) : colorSpecFromRole(iconRole));

  if (m_name != nullptr) {
    m_name->setText(name);
    m_name->setVisible(!name.empty());
    m_name->setColor(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)));
  }
  if (m_battery != nullptr && m_batteryGlyph != nullptr) {
    const ColorRole role = !hasBattery ? ColorRole::OnSurface
        : d->charging                  ? ColorRole::Primary
        : d->battery <= kLowBattery    ? ColorRole::Error
                                       : ColorRole::OnSurface;
    m_batteryGlyph->setVisible(hasBattery);
    m_battery->setVisible(hasBattery);
    if (hasBattery) {
      m_batteryGlyph->setGlyph(phone::batteryGlyph(d->battery, d->charging));
      m_batteryGlyph->setColor(colorSpecFromRole(role));
      m_battery->setText(std::format("{}%", d->battery));
      m_battery->setColor(colorSpecFromRole(role));
    }
  }

  if (auto* area = static_cast<InputArea*>(root()); area != nullptr) {
    std::vector<TooltipRow> rows;
    if (m_service == nullptr || !m_service->available()) {
      rows.push_back({i18n::tr("okaru.phone.title"), i18n::tr("okaru.phone.no-daemon")});
    } else if (d == nullptr) {
      rows.push_back({i18n::tr("okaru.phone.title"), i18n::tr("okaru.phone.no-devices")});
    } else {
      std::string status = usable ? i18n::tr("okaru.phone.connected") : i18n::tr("okaru.phone.offline");
      if (hasBattery) {
        status += std::format(" · {}%", d->battery);
      }
      rows.push_back({name, std::move(status)});
    }
    area->setTooltip(std::move(rows));
  }
  (void)renderer;
  requestUpdate(); // el ancho cambia con el nombre y la batería
}
