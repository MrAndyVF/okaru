#pragma once

// OKARU: listas de la barra lateral de okaru-pdf (índice y miniaturas). Son listas virtuales: solo existen las filas
// visibles, así un libro con mil entradas o cientos de páginas abre sin trabarse.

#include "apps/pdf/pdf_view.h"
#include "ui/controls/virtual_list_view.h"

#include <functional>

namespace okaru::pdf {

  class OutlineAdapter final : public VirtualListAdapter {
  public:
    OutlineAdapter(PdfTab& tab, float scale, std::function<void(int page)> onPage)
        : m_tab(tab), m_scale(scale), m_onPage(std::move(onPage)) {}

    [[nodiscard]] std::size_t itemCount() const override { return m_tab.outline.size(); }
    [[nodiscard]] bool itemInteractive(std::size_t index) const override { return m_tab.outline[index].page >= 0; }
    [[nodiscard]] float measureItem(Renderer& renderer, std::size_t index, float width) override;
    [[nodiscard]] std::unique_ptr<Node> createItem() override;
    void bindItem(Renderer& renderer, Node& item, std::size_t index, float width, bool hovered) override;
    void onActivate(std::size_t index) override { m_onPage(m_tab.outline[index].page); }

  private:
    PdfTab& m_tab;
    float m_scale;
    std::function<void(int)> m_onPage;
  };

  class ThumbnailAdapter final : public VirtualListAdapter {
  public:
    ThumbnailAdapter(PdfTab& tab, float scale, std::function<void(int page)> onPage)
        : m_tab(tab), m_scale(scale), m_onPage(std::move(onPage)) {}

    [[nodiscard]] std::size_t itemCount() const override { return m_tab.sizes.size(); }
    // Cambia cuando llega la imagen o la página pasa a ser la actual: la fila se vuelve a dibujar.
    [[nodiscard]] std::uint64_t itemRevision(std::size_t index) const override;
    [[nodiscard]] bool itemInteractive(std::size_t /*index*/) const override { return true; }
    [[nodiscard]] float measureItem(Renderer& renderer, std::size_t index, float width) override;
    [[nodiscard]] std::unique_ptr<Node> createItem() override;
    void bindItem(Renderer& renderer, Node& item, std::size_t index, float width, bool hovered) override;
    void onActivate(std::size_t index) override { m_onPage(static_cast<int>(index)); }

  private:
    [[nodiscard]] float thumbHeight(std::size_t index) const;

    PdfTab& m_tab;
    float m_scale;
    std::function<void(int)> m_onPage;
  };

} // namespace okaru::pdf
