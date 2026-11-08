#include "shell/bar/widgets/okaru_capture_widget.h"

#include "capture/okaru_recorder.h"
#include "core/timer_manager.h"
#include "i18n/i18n.h"
#include "render/scene/input_area.h"
#include "ui/builders.h"
#include "ui/controls/glyph.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <linux/input-event-codes.h>
#include <vector>

using okaru::capture::Recorder;
using okaru::capture::RecorderState;

OkaruCaptureWidget::OkaruCaptureWidget(wl_output* /*output*/, Options /*options*/)
    : m_tick(std::make_unique<Timer>()) {}

OkaruCaptureWidget::~OkaruCaptureWidget() {
  if (m_listener != 0) {
    Recorder::instance().removeListener(m_listener);
  }
}

void OkaruCaptureWidget::create() {
  auto area = ui::inputArea({});
  area->addChild(
      ui::glyph({
          .out = &m_glyph,
          .glyph = "capture",
          .glyphSize = Style::baseGlyphSize * m_contentScale,
          .color = widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)),
      })
  );
  area->addChild(
      ui::label({
          .out = &m_time,
          .fontSize = Style::fontSizeBody * fontScale(),
          .fontWeight = labelFontWeight(),
          .fontFamily = labelFontFamily(),
          .visible = false,
      })
  );
  // Botones propios: su InputArea recibe el clic antes que la del widget (que abre el menú).
  const auto addButton = [&](InputArea*& outArea, Glyph*& outGlyph, std::string_view glyph, auto onClick) {
    auto button = ui::inputArea({});
    button->addChild(ui::glyph({
        .out = &outGlyph,
        .glyph = std::string(glyph),
        .glyphSize = Style::baseGlyphSize * m_contentScale,
        .color = widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)),
    }));
    button->setOnClick([onClick](const InputArea::PointerData& data) {
      if (data.button == BTN_LEFT) {
        onClick();
      }
    });
    button->setVisible(false);
    outArea = static_cast<InputArea*>(area->addChild(std::move(button)));
  };
  addButton(m_pauseArea, m_pauseGlyph, "player-pause", [] { Recorder::instance().togglePause(); });
  addButton(m_stopArea, m_stopGlyph, "player-stop", [] { Recorder::instance().stop(); });
  setRoot(std::move(area));
  m_listener = Recorder::instance().addListener([this] { syncState(); });
  syncState();
}

void OkaruCaptureWidget::syncState() {
  const auto& recorder = Recorder::instance();
  const RecorderState state = recorder.state();
  const bool recording = state == RecorderState::Recording;
  const auto secs = recorder.elapsed().count();
  const std::string time =
      secs >= 3600 ? std::format("{}:{:02}:{:02}", secs / 3600, (secs / 60) % 60, secs % 60)
                   : std::format("{:02}:{:02}", secs / 60, secs % 60);
  if (m_glyph != nullptr) {
    m_glyph->setGlyph(
        recording                            ? "player-record-filled"
            : state == RecorderState::Replay ? "history"
                                             : "capture"
    );
    m_glyph->setColor(
        state == RecorderState::Idle ? widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface))
                                     : colorSpecFromRole(ColorRole::Error)
    );
    // En pausa el punto rojo se atenúa, como el contador.
    m_glyph->setOpacity(recording && recorder.paused() ? 0.45F : 1.0F);
  }
  if (m_time != nullptr) {
    m_time->setVisible(recording);
    m_time->setText(time);
    m_time->setColor(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)));
    m_time->setOpacity(recorder.paused() ? 0.45F : 1.0F);
  }
  if (m_pauseArea != nullptr && m_pauseGlyph != nullptr) {
    m_pauseArea->setVisible(recording);
    m_pauseGlyph->setGlyph(recorder.paused() ? "player-play" : "player-pause");
    m_pauseArea->setTooltip(i18n::tr(recorder.paused() ? "okaru.capture.resume" : "okaru.capture.pause"));
  }
  if (m_stopArea != nullptr) {
    m_stopArea->setVisible(recording);
    m_stopArea->setTooltip(i18n::tr("okaru.capture.finish"));
  }
  if (auto* area = static_cast<InputArea*>(root()); area != nullptr) {
    area->setTooltip(
        recording ? i18n::tr(recorder.paused() ? "okaru.capture.tooltip-paused" : "okaru.capture.tooltip-recording",
                             "time", time)
            : state == RecorderState::Replay ? i18n::tr("okaru.capture.tooltip-replay")
                                             : i18n::tr("okaru.capture.tooltip")
    );
  }
  if (state == RecorderState::Idle) {
    m_tick->stop();
  } else if (!m_tick->active()) {
    m_tick->startRepeating(std::chrono::seconds(1), [this] { syncState(); });
  }
  // El ancho cambia al empezar o terminar la grabación (aparecen el tiempo y los botones).
  if (root() != nullptr) {
    root()->markLayoutDirty();
  }
  requestUpdate();
}

void OkaruCaptureWidget::doLayout(Renderer& renderer, float containerWidth, float containerHeight) {
  auto* node = root();
  if (node == nullptr || m_glyph == nullptr) {
    return;
  }
  m_isVertical = containerHeight > containerWidth;
  const float glyphSize = Style::baseGlyphSize * m_contentScale;
  m_glyph->setGlyphSize(glyphSize);
  m_glyph->measure(renderer);

  // Elementos visibles en orden: icono, tiempo, pausar, terminar.
  std::vector<Node*> items{m_glyph};
  if (m_time != nullptr && m_time->visible()) {
    m_time->setFontSize((m_isVertical ? Style::fontSizeMini : Style::fontSizeBody) * fontScale());
    m_time->measure(renderer);
    items.push_back(m_time);
  }
  for (auto [area, glyph] : {std::pair{m_pauseArea, m_pauseGlyph}, std::pair{m_stopArea, m_stopGlyph}}) {
    if (area != nullptr && glyph != nullptr && area->visible()) {
      glyph->setGlyphSize(glyphSize);
      glyph->measure(renderer);
      glyph->setPosition(0.0F, 0.0F);
      area->setSize(glyph->width(), glyph->height());
      items.push_back(area);
    }
  }

  const float gap = Style::spaceXs;
  float cross = 0.0F;
  for (const Node* item : items) {
    cross = std::max(cross, m_isVertical ? item->width() : item->height());
  }
  float along = 0.0F;
  for (Node* item : items) {
    if (along > 0.0F) {
      along += gap;
    }
    if (m_isVertical) {
      item->setPosition(std::round((cross - item->width()) * 0.5F), along);
      along += item->height();
    } else {
      item->setPosition(along, std::round((cross - item->height()) * 0.5F));
      along += item->width();
    }
  }
  if (m_isVertical) {
    node->setSize(cross, along);
  } else {
    node->setSize(along, cross);
  }
}
