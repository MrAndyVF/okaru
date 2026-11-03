#include "shell/okaru_audio/okaru_audio_device_row.h"

#include "pipewire/pipewire_service.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/label.h"
#include "ui/controls/radio_button.h"
#include "ui/controls/slider.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace {
  constexpr auto kCommitInterval = std::chrono::milliseconds(16); // como el AudioTab de upstream
  constexpr float kSyncEpsilon = 0.005F;
} // namespace

OkaruAudioDeviceRow::OkaruAudioDeviceRow(
    PipeWireService* audio, const AudioNode& node, bool input, float scale, float sliderMax,
    std::unique_ptr<Node> trailing
)
    : m_audio(audio), m_id(node.id), m_input(input) {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  setGap(Style::spaceXs * scale);
  setPadding(Style::spaceSm * scale, Style::spaceMd * scale);
  setFill(colorSpecFromRole(ColorRole::Surface, 0.4F)); // misma opacidad que las tarjetas de los ajustes
  clearBorder();

  auto top = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
  top->addChild(ui::radioButton({
      .out = &m_radio,
      .checked = node.isDefault,
      .scale = scale,
      .onChange = [this](bool) {
        if (m_audio == nullptr) {
          return;
        }
        if (m_input) {
          m_audio->setDefaultSource(m_id);
        } else {
          m_audio->setDefaultSink(m_id);
        }
        m_radio->setChecked(true);
        if (m_onDefaultChosen) {
          m_onDefaultChosen(m_id);
        }
      },
  }));
  top->addChild(ui::label({
      .out = &m_name,
      .text = audioDeviceLabel(node),
      .fontSize = Style::fontSizeBody * scale,
      .fontWeight = FontWeight::Bold,
      .color = colorSpecFromRole(ColorRole::OnSurface),
      .maxLines = 1,
      .ellipsize = TextEllipsize::Middle,
      .flexGrow = 1.0F,
  }));
  if (trailing != nullptr) {
    top->addChild(std::move(trailing));
  }
  addChild(std::move(top));

  auto bottom = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs * scale});
  bottom->addChild(ui::slider({
      .out = &m_slider,
      .minValue = 0.0F,
      .maxValue = sliderMax,
      .step = 0.01F,
      .value = std::clamp(node.volume, 0.0F, sliderMax),
      .trackHeight = Style::sliderTrackHeight * scale,
      .thumbSize = Style::sliderThumbSize * scale,
      .controlHeight = Style::controlHeight * scale,
      .flexGrow = 1.0F,
      .onValueChanged = [this](double value) {
        if (m_syncing) {
          return;
        }
        sendVolume(static_cast<float>(value), false);
        m_value->setText(std::format("{:.0F}%", value * 100.0));
      },
      .onDragEnd = [this] { sendVolume(m_pending, true); },
  }));
  bottom->addChild(ui::label({
      .out = &m_value,
      .text = std::format("{:.0F}%", node.volume * 100.0F),
      .fontSize = Style::fontSizeBody * scale,
      .fontWeight = FontWeight::Bold,
      .minWidth = (Style::controlHeightLg + Style::spaceLg) * scale,
      .textAlign = TextAlign::End,
  }));
  bottom->addChild(ui::button({
      .out = &m_mute,
      .glyph = std::string(input ? "microphone" : "volume-high"),
      .glyphSize = Style::fontSizeBody * scale,
      .minWidth = Style::controlHeightSm * scale,
      .minHeight = Style::controlHeightSm * scale,
      .padding = Style::spaceXs * scale,
      .onClick = [this] {
        if (m_audio == nullptr) {
          return;
        }
        const bool next = !m_muted;
        if (m_input) {
          m_audio->setSourceMuted(m_id, next);
        } else {
          m_audio->setSinkMuted(m_id, next);
        }
        applyMuted(next); // optimista: los ajustes no reciben cambios de PipeWire en vivo
      },
  }));
  addChild(std::move(bottom));
  applyMuted(node.muted);
}

void OkaruAudioDeviceRow::sync(const AudioNode& node, float sliderMax) {
  m_name->setText(audioDeviceLabel(node));
  m_radio->setChecked(node.isDefault);
  applyMuted(node.muted);
  if (m_slider->dragging()) {
    return;
  }
  m_syncing = true;
  m_slider->setRange(0.0F, sliderMax);
  const float shown = std::clamp(node.volume, 0.0F, sliderMax);
  if (std::abs(static_cast<float>(m_slider->value()) - shown) >= kSyncEpsilon) {
    m_slider->setValue(shown);
    m_value->setText(std::format("{:.0F}%", shown * 100.0F));
  }
  m_syncing = false;
}

void OkaruAudioDeviceRow::setDefaultMark(bool isDefault) { m_radio->setChecked(isDefault); }

bool OkaruAudioDeviceRow::dragging() const noexcept { return m_slider != nullptr && m_slider->dragging(); }

void OkaruAudioDeviceRow::sendVolume(float value, bool force) {
  if (m_audio == nullptr || value < 0.0F) {
    return;
  }
  m_pending = value;
  const auto now = std::chrono::steady_clock::now();
  if (!force && now - m_lastSentAt < kCommitInterval) {
    return; // el último valor sale al soltar (onDragEnd)
  }
  if (m_input) {
    m_audio->setSourceVolume(m_id, value);
  } else {
    m_audio->setSinkVolume(m_id, value);
  }
  m_audio->emitVolumePreview(m_input, m_id, value);
  m_lastSentAt = now;
}

void OkaruAudioDeviceRow::applyMuted(bool muted) {
  m_muted = muted;
  if (m_input) {
    m_mute->setGlyph(muted ? "microphone-mute" : "microphone");
  } else {
    m_mute->setGlyph(muted ? "volume-mute" : "volume-high");
  }
  m_mute->setVariant(muted ? ButtonVariant::Destructive : ButtonVariant::Default);
}
