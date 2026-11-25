#pragma once

#include "ui/controls/flex.h"

// OKARU: tarjetas en una o dos columnas según el ancho disponible. Los hijos se reparten por índice
// (pares a la izquierda, impares a la derecha) para que una tarjeta que se expande no salte de columna.
class CardColumns : public Flex {
public:
  CardColumns(float minColumnWidth, float gap);

  [[nodiscard]] static int columnsFor(float width, float minColumnWidth, float gap) noexcept;

protected:
  void doLayout(Renderer& renderer) override;
  LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;
  void doArrange(Renderer& renderer, const LayoutRect& rect) override;

private:
  float place(Renderer& renderer, float width, bool arrange);

  float m_minColumnWidth;
  float m_gap;
};
