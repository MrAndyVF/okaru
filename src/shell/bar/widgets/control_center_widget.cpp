#include "shell/bar/widgets/control_center_widget.h"

#include "dbus/bluetooth/bluetooth_service.h" // OKARU
#include "dbus/network/inetwork_service.h"    // OKARU
#include "dbus/network/network_display.h"     // OKARU
#include "pipewire/audio_glyphs.h"            // OKARU
#include "pipewire/pipewire_service.h"        // OKARU
#include "render/scene/input_area.h"

#include <linux/input-event-codes.h>
#include "render/scene/node.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

ControlCenterWidget::ControlCenterWidget(
    wl_output* /*output*/, Options options, INetworkService* network, BluetoothService* bluetooth, PipeWireService* audio
)
    : m_barGlyphId(std::move(options.glyph)),
      m_customImage(widget_custom_image::fromConfig(options.customImage, options.customImageColorize)),
      m_network(network), m_bluetooth(bluetooth), m_audio(audio) {}

void ControlCenterWidget::create() {
  auto area = ui::inputArea({});
  // OKARU: estado de red, bluetooth y sonido en la misma cápsula.
  auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * m_contentScale});
  // Clic derecho en un icono de estado abre su panel (Red, Bluetooth, Sonido); el clic izquierdo en cualquier
  // parte pasa al gesto del widget, que abre el centro de control.
  const auto addStatus = [&](Glyph** out, const char* glyph, const char* panelId) {
    auto statusArea = ui::inputArea({});
    statusArea->addChild(ui::glyph({
        .out = out,
        .glyph = glyph,
        .glyphSize = Style::baseGlyphSize * m_contentScale,
        .color = widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)),
    }));
    statusArea->setAcceptedButtons(InputArea::buttonMask(BTN_RIGHT));
    statusArea->setOnClick([this, panelId](const InputArea::PointerData& data) {
      if (data.button == BTN_RIGHT) {
        requestPanelToggle(panelId);
      }
    });
    row->addChild(std::move(statusArea));
  };
  if (m_network != nullptr) {
    addStatus(&m_networkGlyph, "wifi", "okaru-network");
  }
  if (m_bluetooth != nullptr) {
    addStatus(&m_bluetoothGlyph, "bluetooth", "okaru-bluetooth");
  }
  if (m_audio != nullptr) {
    addStatus(&m_volumeGlyph, "volume", "okaru-audio");
  }

  if (m_customImage.enabled()) {
    row->addChild(ui::image({.out = &m_image, .fit = ImageFit::Contain}));
  } else {
    row->addChild(
        ui::glyph({
            .out = &m_glyph,
            .glyph = m_barGlyphId,
            .glyphSize = Style::baseGlyphSize * m_contentScale,
            .color = widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)),
        })
    );
  }
  area->addChild(std::move(row));

  setRoot(std::move(area));
}

// OKARU: los iconos de estado siguen a sus servicios.
void ControlCenterWidget::doUpdate(Renderer& /*renderer*/) {
  bool changed = false;
  if (m_networkGlyph != nullptr) {
    changed |= m_networkGlyph->setGlyph(network_display::glyphForState(m_network->state()));
  }
  if (m_bluetoothGlyph != nullptr) {
    const auto& s = m_bluetooth->state();
    const bool connected = std::ranges::any_of(m_bluetooth->devices(), [](const auto& d) { return d.connected; });
    changed |= m_bluetoothGlyph->setGlyph(
        !s.adapterPresent || !s.powered ? "bluetooth-off" : (connected ? "bluetooth-connected" : "bluetooth")
    );
  }
  if (m_volumeGlyph != nullptr) {
    const auto* sink = m_audio->defaultSink();
    changed |= m_volumeGlyph->setGlyph(audioVolumeGlyph(sink != nullptr ? sink->volume : 0.0F, sink != nullptr && sink->muted, false));
  }
  if (changed) {
    requestRedraw();
  }
}

void ControlCenterWidget::doLayout(Renderer& renderer, float /*containerWidth*/, float /*containerHeight*/) {
  auto* node = root();
  if (node == nullptr) {
    return;
  }
  const ColorSpec color = widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface));
  // OKARU: iconos de estado y el del centro de control, uno al lado del otro.
  std::vector<Node*> items;
  for (Glyph* glyph : {m_networkGlyph, m_bluetoothGlyph, m_volumeGlyph, m_glyph}) {
    if (glyph != nullptr) {
      glyph->setGlyphSize(Style::baseGlyphSize * m_contentScale);
      glyph->setColor(color);
      glyph->measure(renderer);
      items.push_back(glyph);
    }
  }
  if (m_image != nullptr) {
    widget_custom_image::sync(*m_image, renderer, m_customImage, m_contentScale, color);
    items.push_back(m_image);
  }
  const float gap = Style::spaceSm * m_contentScale;
  float height = 0.0F;
  for (const Node* item : items) {
    height = std::max(height, item->height());
  }
  float x = 0.0F;
  for (Node* item : items) {
    // Los iconos de estado van dentro de su área de clic: se ubica el área y el icono queda en su origen.
    Node* target = item->parent() != nullptr && item->parent() != node && item->parent()->parent() != node
        && dynamic_cast<InputArea*>(item->parent()) != nullptr
        ? item->parent()
        : item;
    item->setPosition(0.0F, 0.0F);
    if (target != item) {
      target->setSize(item->width(), item->height());
    }
    target->setPosition(x, std::round((height - item->height()) * 0.5F));
    x += item->width() + gap;
  }
  const float width = items.empty() ? 0.0F : x - gap;
  if (!node->children().empty()) {
    node->children().front()->setSize(width, height); // la fila
  }
  node->setSize(width, height);
}
