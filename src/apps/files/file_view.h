#pragma once

// OKARU: vista de una carpeta en okaru-files. Un solo nodo dibuja la lista (con columnas ordenables) o la
// cuadrícula de iconos, y solo crea las filas visibles. Maneja la selección (clic, Ctrl, Mayús, rectángulo),
// el teclado (flechas, Inicio/Fin, páginas, búsqueda al escribir) y arrastrar elementos sobre carpetas.

#include "apps/files/file_model.h"
#include "render/core/texture_handle.h"
#include "render/scene/node.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

class Box;
class Glyph;
class Image;
class InputArea;
class Label;
class Renderer;
class Scrollbar;

namespace okaru::files {

  enum class ViewMode : std::uint8_t { List, Grid, Compact };

  // Estado que sobrevive a las reconstrucciones de la escena (vive en la pestaña).
  struct ViewState {
    std::set<std::string> selected; // URI
    std::optional<std::size_t> cursor;
    std::optional<std::size_t> anchor; // inicio de la selección con Mayús
    float scrollY = 0.0F;
    std::string revealUri; // al cargar, llevar el cursor a este elemento
  };

  struct DropRequest {
    std::vector<std::string> uris;
    std::string targetUri; // carpeta destino; vacío = fuera de la vista
    float sceneX = 0.0F;
    float sceneY = 0.0F;
    std::uint32_t modifiers = 0;
  };

  class FileView : public Node {
  public:
    using IconFn = std::function<TextureHandle(Renderer&, const FileEntry&, int pixels)>;

    FileView();
    ~FileView() override;

    void setEntries(const std::vector<FileEntry>* entries, ViewState* state);
    void setMode(ViewMode mode) {
      m_mode = mode;
      markLayoutDirty();
    }
    void setScale(float scale) { m_scale = scale; }
    void setIconSize(float size) {
      m_iconSize = size;
      markLayoutDirty();
    }
    void setSort(SortKey key, bool descending) {
      m_sort = key;
      m_descending = descending;
    }
    void setShowLocation(bool show) { m_showLocation = show; }
    void setDimmed(std::set<std::string> uris) { m_dimmed = std::move(uris); } // cortados
    void setActive(bool active) { m_active = active; }                           // columnas: la que tiene foco
    void setIconProvider(IconFn provider) { m_icons = std::move(provider); }
    void setEmptyText(std::string text) { m_emptyText = std::move(text); }

    void setOnActivate(std::function<void(std::size_t)> callback) { m_onActivate = std::move(callback); }
    void setOnSelectionChanged(std::function<void()> callback) { m_onSelectionChanged = std::move(callback); }
    // Clic secundario: índice bajo el puntero (o ninguno) y posición en la escena.
    void setOnContextMenu(std::function<void(std::optional<std::size_t>, float, float)> callback) {
      m_onContextMenu = std::move(callback);
    }
    void setOnSortChanged(std::function<void(SortKey)> callback) { m_onSortChanged = std::move(callback); }
    void setOnDrop(std::function<void(const DropRequest&)> callback) { m_onDrop = std::move(callback); }
    // Durante un arrastre: posición en la escena (para resaltar destinos fuera de la vista).
    void setOnDragMotion(std::function<void(float, float)> callback) { m_onDragMotion = std::move(callback); }
    // Clic simple sin teclas sobre un elemento (la vista en columnas entra en carpetas así).
    void setOnSingleClick(std::function<void(std::size_t)> callback) { m_onSingleClick = std::move(callback); }
    void setOnMiddleClick(std::function<void(std::size_t)> callback) { m_onMiddleClick = std::move(callback); }
    void setOnBack(std::function<void(bool forward)> callback) { m_onBack = std::move(callback); }
    void setOnZoom(std::function<void(int)> callback) { m_onZoom = std::move(callback); }

    [[nodiscard]] InputArea* inputArea() const noexcept { return m_input; }
    void selectAll();
    void invertSelection();
    void clearSelection();
    void selectUris(const std::vector<std::string>& uris);
    void moveCursor(int delta, bool extend);
    void ensureVisible(std::size_t index);
    [[nodiscard]] std::vector<std::size_t> selectedIndices() const;
    [[nodiscard]] bool dragging() const noexcept { return m_dragging; }
    [[nodiscard]] int columnsCount() const { return m_mode == ViewMode::Grid ? columns() : 1; }

    void setSize(float width, float height) override;

  protected:
    void doLayout(Renderer& renderer) override;

  private:
    struct Slot;

    [[nodiscard]] float headerHeight() const;
    [[nodiscard]] float rowHeight() const;
    [[nodiscard]] int columns() const;
    [[nodiscard]] float cellWidth() const;
    [[nodiscard]] float cellHeight() const;
    [[nodiscard]] float contentHeight() const;
    [[nodiscard]] std::optional<std::size_t> indexAt(float x, float y) const;
    [[nodiscard]] std::pair<float, float> itemOrigin(std::size_t index) const; // relativo al contenido
    [[nodiscard]] std::pair<float, float> itemSize() const;
    void clampScroll();
    void scrollBy(float dy);
    void applyPressSelection(std::size_t index, std::uint32_t modifiers);
    void updateRubberBand();
    void typeAhead(std::uint32_t codepoint);
    void notifySelection();

    void onPress(float x, float y, float sceneX, float sceneY, std::uint32_t button, std::uint32_t time);
    void onMotion(float x, float y, float sceneX, float sceneY);
    void onRelease(float x, float y, float sceneX, float sceneY);

    const std::vector<FileEntry>* m_entries = nullptr;
    ViewState* m_state = nullptr;
    ViewMode m_mode = ViewMode::List;
    float m_scale = 1.0F;
    float m_iconSize = 48.0F; // cuadrícula; la lista usa un tamaño fijo
    SortKey m_sort = SortKey::Name;
    bool m_descending = false;
    bool m_showLocation = false;
    bool m_active = true;
    std::set<std::string> m_dimmed;
    std::string m_emptyText;
    IconFn m_icons;

    InputArea* m_input = nullptr;
    Scrollbar* m_vbar = nullptr;
    Node* m_content = nullptr;
    Node* m_header = nullptr;
    Box* m_band = nullptr;
    Label* m_empty = nullptr;
    std::vector<std::unique_ptr<Slot>> m_slots;
    std::vector<Label*> m_headerLabels;
    std::vector<Box*> m_headerHits;

    // Puntero.
    bool m_pressed = false;
    bool m_banding = false;
    bool m_dragging = false;
    bool m_pressOnSelected = false;
    std::optional<std::size_t> m_pressIndex;
    float m_pressX = 0.0F;
    float m_pressY = 0.0F; // en coordenadas del contenido (con desplazamiento)
    float m_pointerX = 0.0F;
    float m_pointerY = 0.0F;
    std::uint32_t m_pressModifiers = 0;
    std::set<std::string> m_bandBase; // selección antes del rectángulo (Ctrl lo suma)
    std::optional<std::size_t> m_dropIndex;
    std::uint32_t m_lastClickTime = 0;
    std::optional<std::size_t> m_lastClickIndex;
    std::optional<std::size_t> m_hover;

    std::string m_typed;
    std::uint32_t m_typedTime = 0;

    std::function<void(std::size_t)> m_onActivate;
    std::function<void()> m_onSelectionChanged;
    std::function<void(std::optional<std::size_t>, float, float)> m_onContextMenu;
    std::function<void(SortKey)> m_onSortChanged;
    std::function<void(const DropRequest&)> m_onDrop;
    std::function<void(float, float)> m_onDragMotion;
    std::function<void(std::size_t)> m_onMiddleClick;
    std::function<void(std::size_t)> m_onSingleClick;
    std::function<void(bool)> m_onBack;
    std::function<void(int)> m_onZoom;
  };

} // namespace okaru::files
