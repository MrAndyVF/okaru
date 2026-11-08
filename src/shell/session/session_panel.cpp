#include "shell/session/session_panel.h"

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/input/keybind_matcher.h"
#include "core/log.h"
#include "i18n/i18n.h"
#include "shell/panel/panel_manager.h"
#include "shell/session/session_action_meta.h"
#include "shell/session/session_action_runner.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/grid_view.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace {

  constexpr Logger kLog("session");
  constexpr float kHoldFillAlpha = 0.58F;

  [[nodiscard]] ButtonVariant buttonVariantFor(SessionActionButtonVariant variant) {
    switch (variant) {
    case SessionActionButtonVariant::Default:
      return ButtonVariant::Default;
    case SessionActionButtonVariant::Primary:
      return ButtonVariant::Primary;
    case SessionActionButtonVariant::Secondary:
      return ButtonVariant::Secondary;
    case SessionActionButtonVariant::Destructive:
      return ButtonVariant::Destructive;
    case SessionActionButtonVariant::Outline:
      return ButtonVariant::Outline;
    case SessionActionButtonVariant::Ghost:
      return ButtonVariant::Ghost;
    }
    return ButtonVariant::Default;
  }

} // namespace

std::vector<SessionPanelActionConfig> SessionPanel::effectiveActions() const {
  std::vector<SessionPanelActionConfig> src =
      m_config != nullptr ? m_config->config().shell.session.actions : defaultSessionPanelActions();

  std::vector<SessionPanelActionConfig> out;
  out.reserve(src.size());
  for (const auto& row : src) {
    if (!row.enabled) {
      continue;
    }
    if (!session_action::isKnown(row.action)) {
      kLog.warn("session panel: skipping unknown action \"{}\"", row.action);
      continue;
    }
    if (row.action == "command" && (!row.command.has_value() || StringUtils::trim(*row.command).empty())) {
      kLog.warn("session panel: skipping \"command\" entry with no command");
      continue;
    }
    if ((row.action == "lock" || row.action == "lock_and_suspend")
        && m_config != nullptr
        && !m_config->isLockScreenEnabled()) {
      continue;
    }
    out.push_back(row);
  }
  return out;
}

PanelPlacement SessionPanel::panelPlacement() const noexcept {
  return m_config != nullptr ? m_config->config().shell.panel.sessionPlacement : PanelPlacement::Attached;
}

float SessionPanel::preferredWidth() const {
  const std::size_t n = visibleColumnCount();
  const float gap = Style::spaceMd;
  const float w = kButtonMinWidth * static_cast<float>(n)
      + gap * static_cast<float>(n > 1 ? n - 1 : 0)
      + Style::panelPadding * 2.0F;
  return scaled(w);
}

float SessionPanel::preferredHeight() const {
  const std::size_t rows = visibleRowCount();
  const float gap = Style::spaceMd;
  const float h = kActionButtonMinHeight * static_cast<float>(rows)
      + gap * static_cast<float>(rows > 1 ? rows - 1 : 0)
      + Style::panelPadding * 2.0F;
  return std::ceil(scaled(h));
}

std::size_t SessionPanel::entryCountForLayout() const {
  if (!m_visibleEntries.empty()) {
    return m_visibleEntries.size();
  }
  return effectiveActions().size();
}

bool SessionPanel::gridEnabled() const { return m_config != nullptr && m_config->config().shell.session.grid; }

std::size_t SessionPanel::visibleColumnCount() const {
  const std::size_t n = std::max<std::size_t>(1, entryCountForLayout());
  if (gridEnabled()) {
    const auto columns = static_cast<std::size_t>(std::max(1, m_config->config().shell.session.gridColumns));
    return std::min(columns, n);
  }
  if (n <= kMaxColumns) {
    return n;
  }
  return std::min<std::size_t>(kMaxColumns, (n + 1) / 2);
}

std::size_t SessionPanel::visibleRowCount() const {
  const std::size_t n = std::max<std::size_t>(1, entryCountForLayout());
  const std::size_t columns = visibleColumnCount();
  return (n + columns - 1) / columns;
}

void SessionPanel::create() {
  const float scale = contentScale();
  m_visibleEntries = effectiveActions();
  const std::size_t columns = visibleColumnCount();

  auto rootLayout = std::make_unique<GridView>();
  rootLayout->setColumns(columns);
  rootLayout->setColumnGap(Style::spaceMd * scale);
  rootLayout->setRowGap(Style::spaceMd * scale);
  rootLayout->setStretchItems(true);
  rootLayout->setUniformCellSize(true);
  rootLayout->setMinCellWidth(kButtonMinWidth * scale);
  rootLayout->setMinCellHeight(kActionButtonMinHeight * scale);
  m_rootLayout = rootLayout.get();

  m_visibleButtons.clear();
  m_holdOverlays.clear();
  m_entryShortcutBadges.clear();
  m_visibleButtons.reserve(m_visibleEntries.size());
  m_holdOverlays.reserve(m_visibleEntries.size());
  m_entryShortcutBadges.reserve(m_visibleEntries.size());
  for (std::size_t i = 0; i < m_visibleEntries.size(); ++i) {
    const auto& cfg = m_visibleEntries[i];
    if (cfg.shortcut.has_value() && cfg.shortcut->sym != 0) {
      m_entryShortcutBadges.emplace_back(keyChordDisplayLabel(*cfg.shortcut));
    } else {
      m_entryShortcutBadges.emplace_back();
    }
    if (Button* b = createActionButton(cfg, i, scale); b != nullptr) {
      HoldOverlay overlay{};
      attachHoldOverlay(*b, overlay, scale);
      m_visibleButtons.push_back(b);
      m_holdOverlays.push_back(overlay);
      rootLayout->addChild(std::unique_ptr<Button>(b));
    }
  }

  setRoot(std::move(rootLayout));

  if (m_animations != nullptr) {
    root()->setAnimationManager(m_animations);
  }

  updateSelectionVisuals();
}

Button* SessionPanel::createActionButton(const SessionPanelActionConfig& cfg, std::size_t index, float scale) {
  const std::string labelText =
      cfg.label.has_value() && !cfg.label->empty() ? *cfg.label : i18n::tr(session_action::labelKey(cfg.action));
  const std::string glyph =
      cfg.glyph.has_value() && !cfg.glyph->empty() ? *cfg.glyph : session_action::defaultGlyph(cfg.action);
  const std::optional<std::string> badge = index < m_entryShortcutBadges.size()
          && m_entryShortcutBadges[index].has_value()
          && m_config->config().shell.session.showShortcuts
      ? m_entryShortcutBadges[index]
      : std::nullopt;

  auto button = ui::button({
      .text = labelText,
      .glyph = glyph,
      .fontSize = (Style::fontSizeBody + 1.0F) * scale,
      .glyphSize = 28.0F * scale,
      .contentAlign = ButtonContentAlign::Center,
      .variant = buttonVariantFor(cfg.variant),
      .surfaceOpacity = panelCardOpacity(),
      .badge = badge,
      .minWidth = kButtonMinWidth * scale,
      .minHeight = kActionButtonMinHeight * scale,
      .paddingV = Style::spaceMd * scale,
      .paddingH = Style::spaceMd * scale,
      .gap = Style::spaceSm * scale,
      .radius = Style::scaledRadiusLg(scale),
      .flexGrow = 1.0F,
      .onPress =
          [this, index](float /*x*/, float /*y*/, bool pressed) {
            if (pressed) {
              beginHold(index, 0);
            } else {
              endHold(index);
            }
          },
      .onLeave = [this, index] { endHold(index); },
      .configure =
          [](Button& control) {
            control.setDirection(FlexDirection::Vertical);
            control.setAlign(FlexAlign::Center);
            control.setJustify(FlexJustify::Center);
            control.setFillHeight(true);
            control.setTabStop(false);
          },
  });
  return button.release();
}

void SessionPanel::attachHoldOverlay(Button& button, HoldOverlay& overlay, float scale) {
  auto overlayRoot = ui::column({
      .out = &overlay.root,
      .align = FlexAlign::Center,
      .justify = FlexJustify::End,
      .clipChildren = true,
      .visible = false,
      .participatesInLayout = false,
      .configure = [](Flex& root) { root.setZIndex(0); },
  });
  overlayRoot->addChild(ui::box({
      .out = &overlay.fill,
      .radius = Style::scaledRadiusLg(scale),
      .participatesInLayout = false,
      .configure = [](Box& box) { box.setZIndex(0); },
  }));
  overlayRoot->addChild(ui::label({
      .out = &overlay.hint,
      .text = i18n::tr("session.hold-to-confirm"),
      .fontSize = Style::fontSizeCaption * scale,
      .fontWeight = FontWeight::Bold,
      .maxLines = 2,
      .textAlign = TextAlign::Center,
      .visible = false,
      .participatesInLayout = false,
      .configure = [](Label& label) { label.setZIndex(1); },
  }));
  button.addChild(std::move(overlayRoot));
}

void SessionPanel::onPanelCardOpacityChanged(float opacity) {
  for (Button* button : m_visibleButtons) {
    if (button != nullptr) {
      button->setSurfaceOpacity(opacity);
    }
  }
}

void SessionPanel::onOpen(std::string_view /*context*/) {
  m_selectedIndex.reset();
  m_hold.reset();
  m_hintIndex.reset();
  m_actionQueued = false;
  updateHoldVisuals();
  updateSelectionVisuals();
}

void SessionPanel::requestHoldFrame() {
  if (root() != nullptr) {
    root()->markPaintDirty();
  }
  PanelManager::instance().requestLayout();
  PanelManager::instance().requestFrameTick();
  PanelManager::instance().refresh();
}

void SessionPanel::beginHold(std::size_t index, std::uint32_t keySym) {
  if (index >= m_visibleEntries.size() || m_actionQueued) {
    return;
  }
  if (m_hold.has_value() && m_hold->index == index) {
    return; // repetición de tecla o segundo press del mismo botón
  }
  m_hold = Hold{.index = index, .elapsedMs = 0.0, .keySym = keySym};
  m_hintIndex.reset();
  m_selectedIndex = index;
  updateSelectionVisuals();
  updateHoldVisuals();
  requestHoldFrame();
}

void SessionPanel::endHold(std::size_t index) {
  if (!m_hold.has_value() || m_hold->index != index) {
    return;
  }
  m_hold.reset();
  m_hintIndex = index;
  m_hintRemainingMs = kHintMs;
  updateSelectionVisuals();
  updateHoldVisuals();
  requestHoldFrame();
}

void SessionPanel::executeEntry(std::size_t index) {
  if (index >= m_visibleEntries.size() || m_actionQueued) {
    return;
  }
  SessionActionRunner* const actionRunner = m_actionRunner;
  if (actionRunner == nullptr) {
    kLog.warn("session panel: action runner unavailable");
    return;
  }

  const SessionPanelActionConfig cfg = m_visibleEntries[index];
  m_actionQueued = true;
  m_hold.reset();
  DeferredCall::callLater([actionRunner, cfg] {
    auto& manager = PanelManager::instance();
    if (cfg.action == "lock" || cfg.action == "lock_and_suspend") {
      manager.closePanelForCapture([actionRunner, cfg] { actionRunner->invoke(cfg); });
      return;
    }
    manager.closePanel(/*animateClose=*/false);
    actionRunner->invoke(cfg);
  });
}

void SessionPanel::updateHoldVisuals() {
  for (std::size_t i = 0; i < m_holdOverlays.size() && i < m_visibleEntries.size(); ++i) {
    HoldOverlay& overlay = m_holdOverlays[i];
    const bool holding = m_hold.has_value() && m_hold->index == i;
    const bool hinting = m_hintIndex.has_value() && *m_hintIndex == i;
    if (overlay.root != nullptr) {
      overlay.root->setVisible(holding || hinting);
    }
    const Button::ButtonStateColors& pressed =
        Button::defaultPalette(buttonVariantFor(m_visibleEntries[i].variant)).pressed;
    if (overlay.fill != nullptr) {
      ColorSpec fill = pressed.bg;
      fill.alpha *= kHoldFillAlpha;
      overlay.fill->setFill(fill);
      overlay.fill->setVisible(holding);
    }
    if (overlay.hint != nullptr) {
      overlay.hint->setColor(pressed.label);
      overlay.hint->setVisible(hinting);
    }
  }
}

void SessionPanel::layoutHoldOverlays(Renderer& renderer) {
  const float progress =
      m_hold.has_value() ? static_cast<float>(std::clamp(m_hold->elapsedMs / kHoldMs, 0.0, 1.0)) : 0.0F;
  for (std::size_t i = 0; i < m_visibleButtons.size() && i < m_holdOverlays.size(); ++i) {
    Button* button = m_visibleButtons[i];
    HoldOverlay& overlay = m_holdOverlays[i];
    if (button == nullptr || overlay.root == nullptr) {
      continue;
    }
    const float width = button->width();
    const float height = button->height();
    overlay.root->setPosition(0.0F, 0.0F);
    overlay.root->setFrameSize(width, height);
    if (overlay.fill != nullptr) {
      // Se llena de izquierda a derecha durante kHoldMs.
      const float fillW = std::round(width * progress);
      overlay.fill->setPosition(0.0F, 0.0F);
      overlay.fill->setFrameSize(fillW, height);
      overlay.fill->setSize(fillW, height);
    }
    if (overlay.hint != nullptr && overlay.hint->visible()) {
      const float pad = Style::spaceSm * contentScale();
      overlay.hint->setMaxWidth(std::max(0.0F, width - 2.0F * pad));
      overlay.hint->measure(renderer);
      overlay.hint->setPosition(
          std::round((width - overlay.hint->width()) * 0.5F), std::round(height - overlay.hint->height() - pad)
      );
    }
  }
}

void SessionPanel::onFrameTick(float deltaMs) {
  bool animating = false;
  if (m_hold.has_value()) {
    m_hold->elapsedMs += static_cast<double>(deltaMs);
    if (m_hold->elapsedMs >= kHoldMs) {
      executeEntry(m_hold->index);
      return;
    }
    animating = true;
  }
  if (m_hintIndex.has_value()) {
    m_hintRemainingMs -= static_cast<double>(deltaMs);
    if (m_hintRemainingMs <= 0.0) {
      m_hintIndex.reset();
      updateHoldVisuals();
    } else {
      animating = true;
    }
  }
  if (animating) {
    requestHoldFrame();
  } else if (root() != nullptr) {
    root()->markPaintDirty();
    PanelManager::instance().refresh();
  }
}

bool SessionPanel::handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) {
  if (preedit) {
    return false;
  }
  // Soltar la tecla con la que se está manteniendo una acción.
  if (!pressed) {
    if (m_hold.has_value() && m_hold->keySym != 0 && m_hold->keySym == sym) {
      endHold(m_hold->index);
      return true;
    }
    return false;
  }

  if (KeybindMatcher::matches(KeybindAction::Cancel, sym, modifiers)) {
    if (m_hold.has_value()) {
      m_hold.reset();
      updateHoldVisuals();
      requestHoldFrame();
      return true;
    }
    return false;
  }

  if (m_visibleButtons.empty()) {
    return false;
  }

  for (std::size_t i = 0; i < m_visibleEntries.size(); ++i) {
    const auto& entryConfig = m_visibleEntries[i];
    if (entryConfig.shortcut.has_value() && keyChordMatches(*entryConfig.shortcut, sym, modifiers)) {
      beginHold(i, sym);
      return true;
    }
  }

  const std::size_t lastIndex = m_visibleButtons.size() - 1;

  const auto moveSelection = [this](std::size_t index) {
    if (m_hold.has_value() && m_hold->index != index) {
      m_hold.reset();
      updateHoldVisuals();
    }
    m_selectedIndex = index;
    updateSelectionVisuals();
    if (root() != nullptr) {
      root()->markPaintDirty();
    }
    PanelManager::instance().refresh();
    return true;
  };

  if (KeybindMatcher::matches(KeybindAction::Left, sym, modifiers)) {
    if (!m_selectedIndex.has_value()) {
      return moveSelection(lastIndex);
    }
    if (*m_selectedIndex > 0) {
      return moveSelection(*m_selectedIndex - 1);
    }
    return true;
  }

  if (KeybindMatcher::matches(KeybindAction::Right, sym, modifiers)) {
    if (!m_selectedIndex.has_value()) {
      return moveSelection(0);
    }
    if (*m_selectedIndex < lastIndex) {
      return moveSelection(*m_selectedIndex + 1);
    }
    return true;
  }

  if (KeybindMatcher::matches(KeybindAction::Up, sym, modifiers)) {
    const std::size_t columns = visibleColumnCount();
    if (!m_selectedIndex.has_value()) {
      return moveSelection(lastIndex);
    }
    if (*m_selectedIndex >= columns) {
      return moveSelection(*m_selectedIndex - columns);
    }
    return true;
  }

  if (KeybindMatcher::matches(KeybindAction::Down, sym, modifiers)) {
    const std::size_t columns = visibleColumnCount();
    if (!m_selectedIndex.has_value()) {
      return moveSelection(0);
    }
    if (*m_selectedIndex + columns <= lastIndex) {
      return moveSelection(*m_selectedIndex + columns);
    }
    return true;
  }

  if (KeybindMatcher::matches(KeybindAction::Validate, sym, modifiers)) {
    if (!m_selectedIndex.has_value()) {
      return false;
    }
    Button* button = m_visibleButtons[*m_selectedIndex];
    if (button != nullptr && button->enabled()) {
      beginHold(*m_selectedIndex, sym);
      return true;
    }
  }

  return false;
}

void SessionPanel::updateSelectionVisuals() {
  for (std::size_t i = 0; i < m_visibleButtons.size(); ++i) {
    Button* button = m_visibleButtons[i];
    if (button == nullptr) {
      continue;
    }
    const bool holdActive = m_hold.has_value() && m_hold->index == i;
    if (holdActive) {
      button->setSelected(false);
      button->setHoveredVisual(false);
      button->setPressedVisual(true);
      continue;
    }

    button->setHoveredVisual(false);
    button->setPressedVisual(false);
    const bool keyboardSelected = m_selectedIndex.has_value() && i == *m_selectedIndex;
    button->setSelected(keyboardSelected);
  }
}

void SessionPanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_rootLayout == nullptr) {
    return;
  }

  m_rootLayout->setSize(width, height);
  m_rootLayout->layout(renderer);

  for (Button* button : m_visibleButtons) {
    if (button != nullptr) {
      button->updateInputArea();
    }
  }
  layoutHoldOverlays(renderer);
}

void SessionPanel::doUpdate(Renderer& /*renderer*/) {}

void SessionPanel::onClose() {
  m_hold.reset();
  m_hintIndex.reset();
  m_selectedIndex.reset();
  m_rootLayout = nullptr;
  m_visibleEntries.clear();
  m_visibleButtons.clear();
  m_holdOverlays.clear();
  m_entryShortcutBadges.clear();
  clearReleasedRoot();
}
