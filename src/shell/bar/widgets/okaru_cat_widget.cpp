#include "shell/bar/widgets/okaru_cat_widget.h"

#include "core/files/resource_paths.h"
#include "core/timer_manager.h"
#include "render/scene/input_area.h"
#include "render/text/font_registry.h"
#include "system/system_monitor_service.h"
#include "ui/builders.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <linux/input-event-codes.h>

namespace {
  constexpr float kTickMs = 80.0F;
  constexpr float kSampleMs = 2000.0F; // el monitor del sistema muestrea cada 2 s por defecto
  constexpr float kWalkMaxMs = 380.0F;
  constexpr float kWalkMinMs = 180.0F;
  constexpr float kRunMaxMs = 160.0F;
  constexpr float kRunMinMs = 55.0F;
  constexpr std::array<const char*, 5> kRunGlyphs{"b", "c", "d", "e", "f"};

  const std::string& catFontFamily() {
    static const std::string family = text::registerFontFile(paths::assetPath("fonts/okaru-cat.otf"));
    return family;
  }
} // namespace

OkaruCatWidget::Pace OkaruCatWidget::paceFor(double cpu, std::int64_t walkThreshold, std::int64_t runThreshold) {
  const double walk = static_cast<double>(walkThreshold);
  const double runFloor = std::max(static_cast<double>(runThreshold), walk + 1.0);
  if (cpu < walk) {
    return {0, kWalkMaxMs};
  }
  if (cpu < runFloor) {
    const double t = std::clamp((cpu - walk) / std::max(runFloor - walk, 1.0), 0.0, 1.0);
    return {1, kWalkMaxMs - static_cast<float>(t) * (kWalkMaxMs - kWalkMinMs)};
  }
  const double t = std::clamp((cpu - runFloor) / std::max(100.0 - runFloor, 1.0), 0.0, 1.0);
  return {2, kRunMaxMs - static_cast<float>(t) * (kRunMaxMs - kRunMinMs)};
}

OkaruCatWidget::OkaruCatWidget(wl_output* /*output*/, Options options, SystemMonitorService* sysmon)
    : m_options(options), m_sysmon(sysmon), m_timer(std::make_unique<Timer>()) {}

OkaruCatWidget::~OkaruCatWidget() = default;

void OkaruCatWidget::create() {
  auto area = ui::inputArea({});
  auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs * m_contentScale});
  row->addChild(ui::label({
      .out = &m_cat,
      .text = "a",
      .fontSize = static_cast<float>(m_options.catSize) * m_contentScale,
      .fontFamily = catFontFamily(),
      .color = colorSpecFromRole(ColorRole::Secondary),
      .baselineMode = LabelBaselineMode::InkCentered,
  }));
  if (m_options.showCpuPercent) {
    row->addChild(ui::label({
        .out = &m_percent,
        .text = "0%",
        .fontSize = Style::fontSizeMini * m_contentScale,
        .color = colorSpecFromRole(ColorRole::Secondary),
    }));
  }
  area->addChild(std::move(row));
  area->setOnClick([this](const InputArea::PointerData& data) {
    if (data.button == BTN_LEFT) {
      requestPanelToggle("okaru-system");
    }
  });
  setRoot(std::move(area));
  m_timer->startRepeating(std::chrono::milliseconds(static_cast<int>(kTickMs)), [this] { tick(); });
  tick();
}

void OkaruCatWidget::tick() {
  m_sampleElapsed += kTickMs;
  if (m_sampleElapsed >= kSampleMs) {
    m_sampleElapsed = 0.0F;
    if (m_sysmon != nullptr && m_sysmon->isRunning()) {
      m_cpu = std::clamp(m_sysmon->latest().cpuUsagePercent, 0.0, 100.0);
    }
  }
  const Pace pace = paceFor(m_cpu, m_options.walkThreshold, m_options.runThreshold);
  if (pace.mode == 0) {
    m_frame = 0;
    m_frameElapsed = 0.0F;
  } else if ((m_frameElapsed += kTickMs) >= pace.frameMs) {
    m_frameElapsed = 0.0F;
    m_frame = (m_frame + 1) % static_cast<int>(kRunGlyphs.size());
  }
  const std::string glyph = pace.mode == 0 ? std::string("a") : std::string(kRunGlyphs[static_cast<std::size_t>(m_frame)]);
  bool changed = false;
  if (m_cat != nullptr && m_cat->text() != glyph) {
    m_cat->setText(glyph);
    changed = true;
  }
  const std::string percent = std::format("{:.0f}%", m_cpu);
  if (m_percent != nullptr && m_percent->text() != percent) {
    m_percent->setText(percent);
    changed = true;
  }
  if (auto* area = static_cast<InputArea*>(root()); area != nullptr) {
    area->setTooltip(std::format("CPU {:.0f}%", m_cpu));
  }
  if (changed) {
    requestRedraw();
  }
}

void OkaruCatWidget::doLayout(Renderer& renderer, float /*containerWidth*/, float /*containerHeight*/) {
  auto* node = root();
  if (node == nullptr || node->children().empty()) {
    return;
  }
  auto* row = static_cast<Flex*>(node->children().front().get());
  row->layout(renderer);
  node->setSize(row->width(), row->height());
}
