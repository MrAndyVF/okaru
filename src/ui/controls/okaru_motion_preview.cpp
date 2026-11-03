#include "ui/controls/okaru_motion_preview.h"

#include "cursor-shape-v1-client-protocol.h"
#include "render/animation/animation_manager.h"
#include "render/core/render_styles.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/rect_node.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <linux/input-event-codes.h>
#include <memory>

namespace {

  constexpr float kPadding = 8.0F;
  constexpr float kPillWidth = 72.0F;
  constexpr float kPillHeight = 36.0F;

  RoundedRectStyle solid(const Color& fill, float radius) {
    return RoundedRectStyle{
        .fill = fill, .border = fill, .fillMode = FillMode::Solid, .radius = radius, .softness = 1.0F
    };
  }

} // namespace

OkaruMotionPreview::OkaruMotionPreview(
    std::function<double(double)> progress, double durationMs, std::string hint, float scale,
    std::shared_ptr<Clock> clock
)
    : m_progress(std::move(progress)), m_durationMs(std::max(50.0, durationMs)), m_scale(scale),
      m_height((kPillHeight + kPadding * 2.0F) * scale), m_clock(clock ? std::move(clock) : std::make_shared<Clock>()) {
  m_paletteConn = paletteChanged().connect([this] { applyPalette(); });

  auto lane = std::make_unique<RectNode>();
  lane->setParticipatesInLayout(false);
  lane->setZIndex(0);
  m_lane = static_cast<RectNode*>(addChild(std::move(lane)));

  auto label = std::make_unique<Label>();
  label->setText(hint);
  label->setFontSize(Style::fontSizeCaption * scale);
  label->setParticipatesInLayout(false);
  label->setHitTestVisible(false);
  label->setZIndex(1);
  m_hint = static_cast<Label*>(addChild(std::move(label)));

  auto pill = std::make_unique<RectNode>();
  pill->setParticipatesInLayout(false);
  pill->setZIndex(2);
  m_pill = static_cast<RectNode*>(addChild(std::move(pill)));

  auto area = std::make_unique<InputArea>();
  area->setParticipatesInLayout(false);
  area->setZIndex(3);
  area->setOnPress([this](const InputArea::PointerData& data) {
    if (data.button == BTN_LEFT && data.pressed) {
      play();
    }
  });
  m_inputArea = static_cast<InputArea*>(addChild(std::move(area)));
  m_inputArea->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);

  // Sigue donde iba la vista previa anterior (o queda en el lado donde terminó). El
  // AnimationManager se asigna al entrar en la escena, así que la reanudación espera al layout.
  m_value = m_clock->towardEnd ? 1.0 : 0.0;
  m_resumePending = m_clock->running;

  applyPalette();
}

OkaruMotionPreview::~OkaruMotionPreview() {
  if (m_animId != 0 && animationManager() != nullptr) {
    animationManager()->cancel(m_animId);
  }
}

void OkaruMotionPreview::play() {
  m_clock->towardEnd = !m_clock->towardEnd;
  m_clock->startedAt = std::chrono::steady_clock::now();
  m_clock->running = true;
  run(0.0);
}

void OkaruMotionPreview::run(double elapsedMs) {
  auto* manager = animationManager();
  if (manager == nullptr) {
    return;
  }
  if (m_animId != 0) {
    manager->cancel(m_animId);
    m_animId = 0;
  }
  const double from = m_clock->towardEnd ? 0.0 : 1.0;
  const double to = m_clock->towardEnd ? 1.0 : 0.0;
  if (elapsedMs >= m_durationMs) {
    m_clock->running = false;
    m_value = to;
    placePill();
    return;
  }
  // Tiempo real (animateTimer): la curva tal cual, sin la velocidad global de la shell.
  m_animId = manager->animateTimer(
      static_cast<float>(elapsedMs), static_cast<float>(m_durationMs), static_cast<float>(m_durationMs - elapsedMs),
      Easing::Linear,
      [this, from, to](float ms) {
        m_value = from + (to - from) * m_progress(ms);
        placePill();
      },
      [this, to] {
        m_animId = 0;
        m_clock->running = false;
        m_value = to;
        placePill();
      },
      this
  );
}

void OkaruMotionPreview::applyPalette() {
  m_lane->setStyle(solid(colorForRole(ColorRole::SurfaceContainerHigh), Style::scaledRadiusMd(m_scale)));
  m_pill->setStyle(solid(colorForRole(ColorRole::Primary), kPillHeight * 0.5F * m_scale));
  m_hint->setColor(colorForRole(ColorRole::OnSurfaceVariant));
  markPaintDirty();
}

void OkaruMotionPreview::placePill() {
  const float pad = kPadding * m_scale;
  const float pillW = kPillWidth * m_scale;
  const float travel = std::max(0.0F, width() - pillW - pad * 2.0F);
  m_pill->setPosition(pad + static_cast<float>(m_value) * travel, pad);
  m_pill->setFrameSize(pillW, kPillHeight * m_scale);
  markPaintDirty();
}

LayoutSize OkaruMotionPreview::doMeasure(Renderer& /*renderer*/, const LayoutConstraints& constraints) {
  const float w = constraints.hasMaxWidth ? constraints.maxWidth : 400.0F * m_scale;
  return constraints.constrain(LayoutSize{.width = w, .height = m_height});
}

void OkaruMotionPreview::doLayout(Renderer& renderer) {
  m_lane->setPosition(0.0F, 0.0F);
  m_lane->setFrameSize(width(), m_height);
  m_inputArea->setPosition(0.0F, 0.0F);
  m_inputArea->setFrameSize(width(), m_height);
  m_hint->measure(renderer);
  m_hint->setPosition(width() - m_hint->width() - kPadding * 2.0F * m_scale, (m_height - m_hint->height()) * 0.5F);
  m_hint->setFrameSize(m_hint->width(), m_hint->height());
  placePill();
  setSize(width(), m_height);
  if (m_resumePending && animationManager() != nullptr) {
    m_resumePending = false;
    const double elapsed =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_clock->startedAt).count();
    run(elapsed);
  }
}
