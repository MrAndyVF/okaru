#pragma once

// OKARU: las tarjetas del centro de control. Cada una es un elemento independiente; el panel solo las
// coloca en la cuadrícula y llama a sync() en cada actualización.

#include "config/config_types.h"
#include "shell/control_center/control_center_services.h"
#include "shell/okaru_dashboard/okaru_dashboard_layout.h"
#include "ui/controls/flex.h"

#include <functional>
#include <memory>

class InputArea;
class Renderer;
class SessionActionRunner;
class Shortcut;

namespace okaru::dashboard {

  struct CardDeps {
    ControlCenterServices services;
    SessionActionRunner* session = nullptr;
  };

  // Flex con un área de clic que lo cubre entero (izquierdo y derecho). Se añade al final, así que queda
  // encima: solo para contenedores sin controles propios.
  class ClickFlex : public Flex {
  public:
    void setActivation(std::function<void()> onClick, std::function<void()> onRightClick = {});

  protected:
    void doLayout(Renderer& renderer) override;
    LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;
    void doArrange(Renderer& renderer, const LayoutRect& rect) override;
    bool m_restBorder = false; // solo las tarjetas tienen borde en reposo

  private:
    void applyHover(bool hovered);
    InputArea* m_area = nullptr;
  };

  class DashboardCard : public ClickFlex {
  public:
    DashboardCard(CardSize size, float scale, float fillOpacity);
    virtual void sync(Renderer& renderer) { (void)renderer; }
    [[nodiscard]] bool compact() const noexcept { return m_size == CardSize::Small; }

  protected:
    CardSize m_size;
    float m_scale;
  };

  // Crea la tarjeta de `type`. `shortcut` es el acceso directo ya creado (lo guarda el panel entre
  // aperturas) cuando el tipo es "shortcut:<id>". nullptr si el tipo no existe.
  [[nodiscard]] std::unique_ptr<DashboardCard> createCard(
      std::string_view type, CardSize size, const CardDeps& deps, float scale, float fillOpacity, Shortcut* shortcut
  );

  // Nombre visible de una tarjeta (para la edición y la lista de "Agregar").
  [[nodiscard]] std::string cardLabel(std::string_view type, const Shortcut* shortcut);

} // namespace okaru::dashboard
