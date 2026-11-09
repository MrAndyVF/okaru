#pragma once

// OKARU: vista de páginas de okaru-pdf. Desplazamiento continuo (una o dos columnas), solo las páginas visibles tienen
// imagen; las texturas, el zoom, la búsqueda y la selección viven en PdfTab (la vista se recrea al reconstruir la
// ventana). Pide al RenderWorker las páginas que faltan o se ven borrosas al nivel de zoom actual.

#include "apps/pdf/pdf_document.h"
#include "apps/pdf/render_worker.h"
#include "render/core/texture_handle.h"
#include "render/scene/node.h"

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class Box;
class Image;
class InputArea;
class Renderer;

namespace okaru::pdf {

  enum class Fit : std::uint8_t { Width, Page, Free };

  struct PageTexture {
    TextureHandle handle;
    RenderJob job; // con qué se dibujó (escala, giro, nocturno)
  };

  struct PdfTab {
    std::filesystem::path path;
    std::string password;
    std::unique_ptr<Document> doc;
    std::unique_ptr<RenderWorker> worker;
    std::vector<PageSize> sizes;
    std::vector<OutlineItem> outline;
    Metadata meta;

    std::vector<PageTexture> pages;
    std::vector<PageTexture> thumbs;
    std::vector<std::pair<RenderJob, RenderedPage>> arrived; // resultados sin subir a la GPU

    Fit fit = Fit::Width;
    double zoom = 1.0; // 1 = tamaño real (96 ppp)
    int rotation = 0;
    bool dual = false;
    bool night = false;
    float scrollY = 0.0F;
    float scrollX = 0.0F;
    int currentPage = 0;
    std::optional<int> pendingPage; // ir a esta página en cuanto se conozca la geometría
    float layoutScale = -1.0F;      // píxeles por punto de la última maquetación
    int anchorPage = 0;             // página bajo el borde de arriba y a qué fracción de ella
    float anchorFraction = 0.0F;

    std::string query;
    std::vector<SearchHit> hits;
    int hitIndex = -1;
    bool searching = false;

    int selectionPage = -1;
    std::size_t selectionFrom = 0;
    std::size_t selectionTo = 0;
    std::map<int, TextLayout> textCache;
    std::map<int, std::vector<Link>> linkCache;

    [[nodiscard]] const TextLayout& text(int page);
    [[nodiscard]] const std::vector<Link>& links(int page);
    [[nodiscard]] std::string selectedText();
  };

  class PdfView : public Node {
  public:
    PdfView();
    ~PdfView() override;

    void setTab(PdfTab* tab) {
      m_tab = tab;
      markLayoutDirty();
    }
    void setPresentation(bool presentation) {
      m_presentation = presentation;
      markLayoutDirty();
    }
    void setBufferScale(float scale) { m_bufferScale = scale; }

    // Navegación desde la ventana.
    void goToPage(int page, std::optional<Rect> show = std::nullopt);
    void scrollBy(float dx, float dy);
    void pageStep(int direction); // pantalla arriba/abajo
    void setZoom(double zoom, std::optional<std::pair<float, float>> anchor = std::nullopt);
    void setFit(Fit fit);
    [[nodiscard]] double effectiveZoom() const;

    [[nodiscard]] InputArea* inputArea() const noexcept { return m_input; }
    void setOnPageChanged(std::function<void()> callback) { m_onPageChanged = std::move(callback); }
    void setOnOpenUri(std::function<void(const std::string&)> callback) { m_onOpenUri = std::move(callback); }
    void setOnZoomChanged(std::function<void()> callback) { m_onZoomChanged = std::move(callback); }
    void setOnSelectionChanged(std::function<void()> callback) { m_onSelectionChanged = std::move(callback); }
    // Presentación: clic avanza.
    void setOnAdvance(std::function<void(int)> callback) { m_onAdvance = std::move(callback); }

    void setSize(float width, float height) override;

  protected:
    void doLayout(Renderer& renderer) override;

  private:
    struct Placed {
      int page = 0;
      float x = 0.0F;
      float y = 0.0F;
      float width = 0.0F;
      float height = 0.0F;
    };

    void computeLayout();
    [[nodiscard]] float pxPerPoint() const;
    [[nodiscard]] PageSize rotatedSize(int page) const;
    void uploadArrived(Renderer& renderer);
    void requestRenders();
    void refresh(Renderer& renderer);
    void clampScroll();
    void updateCurrentPage();
    // Punto de la vista → página y coordenadas en puntos (sin girar).
    [[nodiscard]] std::optional<std::pair<int, std::pair<double, double>>> hit(float x, float y) const;
    [[nodiscard]] Rect toView(const Placed& placed, const Rect& rect) const;
    [[nodiscard]] std::optional<std::size_t> charAt(int page, double px, double py);
    void onPress(float x, float y);
    void onDrag(float x, float y);
    void onRelease(float x, float y);
    void onMotion(float x, float y);

    PdfTab* m_tab = nullptr;
    bool m_presentation = false;
    float m_bufferScale = 1.0F;
    std::vector<Placed> m_placed;
    float m_contentWidth = 0.0F;
    float m_contentHeight = 0.0F;
    int m_firstVisible = 0;
    double m_reportedZoom = -1.0;
    int m_lastVisible = 0;

    InputArea* m_input = nullptr;
    Node* m_content = nullptr;
    std::vector<Box*> m_paper;
    std::vector<Image*> m_images;
    std::vector<Box*> m_marks;
    bool m_selecting = false;
    bool m_pressedOnLink = false;
    std::optional<std::size_t> m_anchorChar;

    std::function<void()> m_onPageChanged;
    std::function<void(const std::string&)> m_onOpenUri;
    std::function<void()> m_onZoomChanged;
    std::function<void()> m_onSelectionChanged;
    std::function<void(int)> m_onAdvance;
  };

  // Zoom real de una página para ajustarla al ancho o al alto disponibles.
  [[nodiscard]] double fitZoom(Fit fit, PageSize size, float width, float height, bool dual);

} // namespace okaru::pdf
