#pragma once

#include "config/config_types.h"
#include "shell/panel/panel.h"
#include "ui/style.h"

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class Box;
class Button;
class Flex;
class GridView;
class InputArea;
class Label;
class Renderer;
class ConfigService;
class SessionActionRunner;

class SessionPanel : public Panel {
public:
  explicit SessionPanel(ConfigService* config, SessionActionRunner& actionRunner)
      : m_config(config), m_actionRunner(&actionRunner) {}

  void create() override;
  void onOpen(std::string_view context) override;
  void onClose() override;
  void onFrameTick(float deltaMs) override;
  [[nodiscard]] bool handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) override;

  [[nodiscard]] float preferredWidth() const override;
  [[nodiscard]] float preferredHeight() const override;
  [[nodiscard]] bool hasDecoration() const override { return true; }
  [[nodiscard]] LayerShellKeyboard keyboardMode() const override { return LayerShellKeyboard::Exclusive; }
  [[nodiscard]] PanelPlacement panelPlacement() const noexcept override;

private:
  // OKARU: como dux-shell, una acción se confirma manteniendo presionado su botón (o su tecla) kHoldMs;
  // el botón se llena mientras tanto y, si se suelta antes, avisa que hay que mantenerlo.
  struct HoldOverlay {
    Flex* root = nullptr;
    Box* fill = nullptr;
    Label* hint = nullptr;
  };

  struct Hold {
    std::size_t index = 0;
    double elapsedMs = 0.0;
    std::uint32_t keySym = 0; // 0 = mouse
  };

  static constexpr double kHoldMs = 300.0;
  static constexpr double kHintMs = 2000.0;

  static constexpr float kActionButtonMinHeight = 112.0F;
  static constexpr float kButtonMinWidth = 152.0F;
  static constexpr std::size_t kMaxColumns = 5;

  void doLayout(Renderer& renderer, float width, float height) override;
  void doUpdate(Renderer& renderer) override;
  void onPanelCardOpacityChanged(float opacity) override;
  void beginHold(std::size_t index, std::uint32_t keySym);
  // Soltar antes de tiempo cancela y muestra el aviso en ese botón.
  void endHold(std::size_t index);
  void executeEntry(std::size_t index);
  void updateSelectionVisuals();
  void updateHoldVisuals();
  void layoutHoldOverlays(Renderer& renderer);
  void attachHoldOverlay(Button& button, HoldOverlay& overlay, float scale);
  void requestHoldFrame();
  [[nodiscard]] std::vector<SessionPanelActionConfig> effectiveActions() const;
  [[nodiscard]] Button* createActionButton(const SessionPanelActionConfig& cfg, std::size_t index, float scale);
  [[nodiscard]] std::size_t entryCountForLayout() const;
  [[nodiscard]] bool gridEnabled() const;
  [[nodiscard]] std::size_t visibleColumnCount() const;
  [[nodiscard]] std::size_t visibleRowCount() const;

  GridView* m_rootLayout = nullptr;
  std::vector<SessionPanelActionConfig> m_visibleEntries;
  std::vector<Button*> m_visibleButtons;
  std::vector<HoldOverlay> m_holdOverlays;
  std::vector<std::optional<std::string>> m_entryShortcutBadges;
  std::optional<std::size_t> m_selectedIndex;
  std::optional<Hold> m_hold;
  std::optional<std::size_t> m_hintIndex;
  double m_hintRemainingMs = 0.0;
  bool m_actionQueued = false;
  ConfigService* m_config = nullptr;
  SessionActionRunner* m_actionRunner = nullptr;
};
