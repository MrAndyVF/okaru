#pragma once

// OKARU: centro de control tipo dashboard. Cuadrícula de 4 columnas con tarjetas independientes
// ([[control_center.cards]]): todas se pueden agregar, quitar, mover y cambiar de tamaño desde el modo
// edición. Registrado como "control-center", así que atajos, IPC y el widget de la barra no cambian.

#include "config/config_types.h"
#include "core/timer_manager.h"
#include "shell/okaru_dashboard/okaru_dashboard_cards.h"
#include "shell/panel/panel.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class Flex;
class ScrollView;
class Shortcut;
class Tab;

class OkaruDashboardPanel : public Panel {
public:
  explicit OkaruDashboardPanel(okaru::dashboard::CardDeps deps);
  ~OkaruDashboardPanel() override;

  // Pestaña que se abre como detalle emergente encima del centro de control (mismo id que su panel suelto).
  void addDetail(std::string id, std::string titleKey, std::string glyph, std::unique_ptr<Tab> tab, float height);

  void create() override;
  void onOpen(std::string_view context) override;
  void onClose() override;
  void onFrameTick(float deltaMs) override;
  [[nodiscard]] bool dismissTransientUi() override;
  [[nodiscard]] bool handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) override;

  [[nodiscard]] float preferredWidth() const override;
  [[nodiscard]] float preferredHeight() const override { return scaled(900.0F); }
  [[nodiscard]] bool fitsContentHeight() const noexcept override { return true; }
  [[nodiscard]] float measureContentHeight(Renderer& renderer, float contentWidth) override;

private:
  enum class Mode : std::uint8_t { View, Edit, Add };

  struct Detail {
    std::string titleKey;
    std::string glyph;
    std::unique_ptr<Tab> tab;
    float height = 560.0F;
  };

  void doLayout(Renderer& renderer, float width, float height) override;
  void doUpdate(Renderer& renderer) override;
  void markDirty();
  void rebuild();
  void rebuildHeader();
  void buildGrid(float scale);
  void buildAddList(float scale);
  std::unique_ptr<Flex> makeEditTile(std::size_t index, okaru::dashboard::CardSize size, float scale);
  void save(std::vector<ControlCenterCardConfig> cards);
  [[nodiscard]] std::vector<ControlCenterCardConfig> currentCards() const;
  [[nodiscard]] Shortcut* shortcutFor(std::string_view type);
  [[nodiscard]] float cellWidth() const;
  void requestDetail(std::string id); // "" = cerrar
  void applyDetail(const std::string& id);
  void closeDetail();
  [[nodiscard]] Detail* activeDetail();

  okaru::dashboard::CardDeps m_deps;
  Flex* m_root = nullptr;
  Flex* m_header = nullptr;
  Flex* m_body = nullptr;
  ScrollView* m_scroll = nullptr; // contiene m_body; se ubica a mano en doLayout
  std::vector<okaru::dashboard::DashboardCard*> m_cards;
  // Los accesos directos viven entre aperturas (los de plugins tienen runtime y vigilancias propias).
  std::unordered_map<std::string, std::unique_ptr<Shortcut>> m_shortcuts;
  PluginsConfig m_lastPlugins;
  std::unordered_map<std::string, Detail> m_details;
  std::string m_activeDetail;
  std::string m_pendingDetail;
  bool m_detailPending = false;
  Flex* m_scrim = nullptr;
  Flex* m_detailCard = nullptr;
  Flex* m_detailBody = nullptr;
  Flex* m_detailContainer = nullptr;
  Mode m_mode = Mode::View;
  bool m_dirty = true;
  bool m_confirmReset = false;
  Timer m_tick;
};
