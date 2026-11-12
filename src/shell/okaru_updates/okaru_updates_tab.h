#pragma once

// OKARU: panel de actualizaciones (okaru-updates), sobre okaru::updates::Engine. Estado y botón Actualizar; log en
// vivo con progreso; pendientes por canal (pacman, AUR, Flatpak, Snap) con ignorar y abrir su página; ignorados;
// gráfico de actividad; e historial con reversión desde la caché de pacman.

#include "shell/control_center/tab.h"

#include <cstdint>
#include <set>
#include <string>

class Flex;

class OkaruUpdatesTab : public Tab {
public:
  OkaruUpdatesTab() = default;
  ~OkaruUpdatesTab() override;

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void setActive(bool active) override;
  void onClose() override;

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void rebuildIfNeeded();
  void requestRebuild();
  [[nodiscard]] std::string stateKey() const;

  void addStatus(Flex& parent);
  void addRun(Flex& parent);
  void addSources(Flex& parent);
  void addIgnored(Flex& parent);
  void addActivity(Flex& parent);
  void addHistory(Flex& parent);
  void addRunView(Flex& parent);

  Flex* m_root = nullptr;
  Flex* m_content = nullptr;
  int m_listener = 0;
  std::string m_key;
  std::set<std::string> m_expanded; // canales abiertos
  std::int64_t m_openRun = 0;       // run del historial abierto (0: ninguno)
  std::string m_armed;              // reversión que espera el segundo clic
};
