#pragma once

// OKARU: control de edición de okaru-edit. Dibuja solo las filas visibles (un Label con markup por fila, con los
// colores del resaltado), números de línea, cursor, selección, línea actual, coincidencias de búsqueda y la línea con
// error. Fuente monoespaciada: cada carácter ocupa celdas fijas, así posición ↔ píxel es una cuenta. Teclado y ratón
// como en GNOME Text Editor; el texto vive en un TextBuffer del documento.

#include "apps/editor/syntax.h"
#include "apps/editor/text_buffer.h"
#include "render/scene/node.h"

#include <functional>
#include <optional>
#include <vector>

class Box;
class InputArea;
class Label;
class Renderer;

namespace okaru::editor {

  struct ViewOptions {
    float fontSize = 14.0F;
    int tabWidth = 4;
    bool insertSpaces = true;
    bool wrap = false;
    bool lineNumbers = true;
    bool highlightLine = true;

    bool operator==(const ViewOptions&) const = default;
  };

  // Resaltado de un documento, con caché por línea. Vive en el documento (no en la vista, que se recrea al
  // reconstruir la ventana) para no volver a analizar un archivo grande cada vez.
  class HighlightCache {
  public:
    void setLanguage(Language language);
    [[nodiscard]] Language language() const noexcept { return m_language; }
    // Llamar antes de pedir tramos: invalida desde la primera línea que cambió en el texto.
    void sync(TextBuffer& buffer);
    [[nodiscard]] const std::vector<Span>& spans(const TextBuffer& buffer, std::size_t line);

  private:
    Language m_language = Language::Text;
    std::vector<LineState> m_states; // estado al empezar cada línea; válido hasta m_statesValid
    std::size_t m_statesValid = 0;
    std::vector<std::vector<Span>> m_spans;
    std::vector<char> m_spansValid;
  };

  class EditorView : public Node {
  public:
    EditorView();
    ~EditorView() override;

    void setBuffer(TextBuffer* buffer, HighlightCache* highlight);
    void setOptions(const ViewOptions& options);
    [[nodiscard]] const ViewOptions& options() const noexcept { return m_options; }

    [[nodiscard]] TextPos cursor() const noexcept { return m_cursor; }
    [[nodiscard]] TextRange selection() const noexcept { return {m_anchor, m_cursor}; }
    [[nodiscard]] std::string selectedText() const;
    void setCursor(TextPos pos, bool extend = false);
    void select(TextRange range); // deja el cursor al final y lo hace visible, centrado si estaba lejos
    void scrollToCursor(bool center = false);
    // Primera fila visible (para recordar dónde quedó un archivo).
    [[nodiscard]] std::size_t topLine() const;
    void setTopLine(std::size_t line);

    void setSearchMatches(std::vector<TextRange> matches);
    void setProblem(std::optional<TextPos> at);

    [[nodiscard]] float scrollY() const noexcept { return m_scrollY; }
    void setScrollY(float scrollY);

    // Edición desde la ventana (menús, buscar y reemplazar).
    void insertText(std::string_view text);
    void undo();
    void redo();
    void selectAll();
    void copy();
    void cut();
    void paste();

    [[nodiscard]] InputArea* inputArea() const noexcept { return m_input; }
    // El texto cambió (para el título, la validación…) o el cursor se movió (barra de estado).
    void setOnChanged(std::function<void()> callback) { m_onChanged = std::move(callback); }
    void setOnCursorMoved(std::function<void()> callback) { m_onCursorMoved = std::move(callback); }
    // Ctrl+rueda: la ventana cambia el tamaño de letra.
    void setOnZoom(std::function<void(int steps)> callback) { m_onZoom = std::move(callback); }

    void setSize(float width, float height) override;

  protected:
    void doLayout(Renderer& renderer) override;

  private:
    struct VisualRow {
      std::size_t line = 0;
      std::size_t start = 0; // byte donde empieza la fila dentro de la línea
      std::size_t end = 0;
    };

    void onKey(std::uint32_t sym, std::uint32_t utf32, std::uint32_t modifiers);
    void onPointer(float x, float y, bool press, bool extend, std::uint32_t time);
    void moveTo(TextPos pos, bool extend, bool keepColumn = false);
    void edited();
    void replaceSelection(std::string_view text);
    void newline();
    void indentLines(bool outdent);
    void toggleComment();
    void moveLines(int direction);
    void deleteLines();
    void duplicateLines();
    void backspace(bool word);
    void deleteForward(bool word);
    void moveVertical(int rows, bool extend);

    // Geometría monoespaciada.
    void measure(Renderer& renderer);
    [[nodiscard]] std::size_t cellsBefore(std::size_t line, std::size_t byte) const;
    [[nodiscard]] std::size_t byteAtCells(std::size_t line, std::size_t from, std::size_t to, std::size_t cells) const;
    void rebuildRows();
    [[nodiscard]] std::size_t rowOf(TextPos pos) const;
    [[nodiscard]] TextPos posAt(float x, float y) const;
    [[nodiscard]] float gutterWidth() const;
    [[nodiscard]] float textLeft() const;
    [[nodiscard]] std::size_t visibleRows() const;
    void clampScroll();

    [[nodiscard]] std::string markupFor(const VisualRow& row);
    void refresh(); // reacomoda los hijos a la geometría y al texto actuales

    TextBuffer* m_buffer = nullptr;
    HighlightCache* m_highlight = nullptr;
    ViewOptions m_options;
    TextPos m_cursor;
    TextPos m_anchor;
    std::optional<std::size_t> m_preferredCells; // columna para ↑/↓
    float m_scrollY = 0.0F;                      // píxeles
    float m_scrollX = 0.0F;
    std::vector<VisualRow> m_rows;
    std::uint64_t m_rowsVersion = static_cast<std::uint64_t>(-1);
    float m_rowsWidth = -1.0F;

    std::vector<TextRange> m_matches;
    std::optional<TextPos> m_problem;

    float m_charWidth = 8.0F;
    float m_lineHeight = 20.0F;
    float m_measuredSize = 0.0F;
    bool m_focused = false;
    bool m_dragging = false;
    std::uint32_t m_lastClick = 0;
    int m_clickCount = 0;

    InputArea* m_input = nullptr;
    Node* m_content = nullptr; // recorta el texto
    Box* m_currentLine = nullptr;
    Box* m_caret = nullptr;
    Box* m_scrollThumb = nullptr;
    std::vector<Box*> m_highlights; // selección, coincidencias y error
    std::vector<Label*> m_lineLabels;
    std::vector<Label*> m_numberLabels;
    Renderer* m_renderer = nullptr;

    std::function<void()> m_onChanged;
    std::function<void()> m_onCursorMoved;
    std::function<void(int)> m_onZoom;
  };

} // namespace okaru::editor
