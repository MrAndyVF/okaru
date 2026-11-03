#include "shell/bar/widgets/okaru_capture_widget.h"

#include "capture/okaru_recorder.h"
#include "core/timer_manager.h"
#include "i18n/i18n.h"
#include "render/scene/input_area.h"
#include "ui/builders.h"
#include "ui/controls/glyph.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <format>

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
  setRoot(std::move(area));
  m_listener = Recorder::instance().addListener([this] { syncState(); });
  syncState();
}

void OkaruCaptureWidget::syncState() {
  const auto& recorder = Recorder::instance();
  const RecorderState state = recorder.state();
  if (m_glyph != nullptr) {
    m_glyph->setGlyph(
        state == RecorderState::Recording    ? "player-record-filled"
            : state == RecorderState::Replay ? "history"
                                             : "capture"
    );
    m_glyph->setColor(
        state == RecorderState::Idle ? widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface))
                                     : colorSpecFromRole(ColorRole::Error)
    );
  }
  if (auto* area = static_cast<InputArea*>(root()); area != nullptr) {
    const auto secs = recorder.elapsed().count();
    area->setTooltip(
        state == RecorderState::Recording
            ? i18n::tr("okaru.capture.tooltip-recording", "time", std::format("{:02}:{:02}", secs / 60, secs % 60))
            : state == RecorderState::Replay ? i18n::tr("okaru.capture.tooltip-replay")
                                             : i18n::tr("okaru.capture.tooltip")
    );
  }
  if (state == RecorderState::Idle) {
    m_tick->stop();
  } else if (!m_tick->active()) {
    m_tick->startRepeating(std::chrono::seconds(1), [this] { syncState(); });
  }
  requestRedraw();
}

void OkaruCaptureWidget::doLayout(Renderer& renderer, float /*containerWidth*/, float /*containerHeight*/) {
  auto* node = root();
  if (node == nullptr || m_glyph == nullptr) {
    return;
  }
  m_glyph->setGlyphSize(Style::baseGlyphSize * m_contentScale);
  m_glyph->measure(renderer);
  node->setSize(m_glyph->width(), m_glyph->height());
}
