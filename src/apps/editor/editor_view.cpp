#include "apps/editor/editor_view.h"

#include "apps/common/okaru_app.h"
#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "core/text_clipboard.h"
#include "render/core/color.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "ui/controls/box.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/palette.h"

#include <algorithm>
#include <cmath>
#include <cursor-shape-v1-client-protocol.h>
#include <format>
#include <limits>
#include <linux/input-event-codes.h>

namespace okaru::editor {

  namespace {

    constexpr std::uint32_t kMultiClickMs = 400;
    constexpr float kGutterPadding = 12.0F;
    constexpr float kTextPadding = 8.0F;
    constexpr std::size_t kMaxHighlights = 400;
    constexpr const char* kFontFamily = "monospace";

    // Colores del resaltado: el primario de la paleta con el tono rotado, así hay variedad (en una paleta clara el
    // primario, el secundario y el terciario se parecen) y todo sigue en armonía con el tema, claro u oscuro.
    struct TokenStyle {
      float hueShift = 0.0F; // grados; NAN = usar `role` tal cual
      ColorRole role = ColorRole::Primary;
      bool bold = false;
      bool italic = false;
    };

    TokenStyle styleFor(Token token) {
      constexpr float kRole = std::numeric_limits<float>::quiet_NaN();
      switch (token) {
      case Token::Keyword:
        return {0.0F, ColorRole::Primary, true};
      case Token::Tag:
      case Token::Key:
        return {0.0F};
      case Token::String:
        return {150.0F};
      case Token::Number:
      case Token::Constant:
        return {60.0F, ColorRole::Primary, token == Token::Constant};
      case Token::Attribute:
      case Token::Function:
        return {300.0F};
      case Token::Section:
        return {210.0F, ColorRole::Primary, true};
      case Token::Comment:
        return {kRole, ColorRole::OnSurfaceVariant, false, true};
      case Token::Punctuation:
        return {kRole, ColorRole::OnSurfaceVariant};
      case Token::Column0:
      case Token::Column1:
      case Token::Column2:
      case Token::Column3:
      case Token::Column4:
      case Token::Column5:
        return {static_cast<float>(static_cast<int>(token) - static_cast<int>(Token::Column0)) * 60.0F};
      case Token::Plain:
        break;
      }
      return {kRole, ColorRole::OnSurface};
    }

    Color rotateHue(const Color& color, float degrees) {
      const float maxC = std::max({color.r, color.g, color.b});
      const float minC = std::min({color.r, color.g, color.b});
      const float light = (maxC + minC) * 0.5F;
      const float delta = maxC - minC;
      float hue = 0.0F;
      float sat = 0.0F;
      if (delta > 1e-5F) {
        sat = delta / (1.0F - std::abs(2.0F * light - 1.0F));
        if (maxC == color.r) {
          hue = 60.0F * std::fmod((color.g - color.b) / delta, 6.0F);
        } else if (maxC == color.g) {
          hue = 60.0F * ((color.b - color.r) / delta + 2.0F);
        } else {
          hue = 60.0F * ((color.r - color.g) / delta + 4.0F);
        }
      }
      // Con poca saturación todos los tonos se verían grises: un mínimo para que se distingan.
      sat = std::max(sat, 0.45F);
      hue = std::fmod(hue + degrees + 360.0F, 360.0F);
      const float c = (1.0F - std::abs(2.0F * light - 1.0F)) * sat;
      const float x = c * (1.0F - std::abs(std::fmod(hue / 60.0F, 2.0F) - 1.0F));
      const float m = light - c * 0.5F;
      float r = 0;
      float g = 0;
      float b = 0;
      if (hue < 60) {
        r = c, g = x;
      } else if (hue < 120) {
        r = x, g = c;
      } else if (hue < 180) {
        g = c, b = x;
      } else if (hue < 240) {
        g = x, b = c;
      } else if (hue < 300) {
        r = x, b = c;
      } else {
        r = c, b = x;
      }
      return rgba(r + m, g + m, b + m, 1.0F);
    }

    Color tokenColor(const TokenStyle& style) {
      return std::isnan(style.hueShift) ? colorForRole(style.role)
                                        : rotateHue(colorForRole(ColorRole::Primary), style.hueShift);
    }

    void appendEscaped(std::string& out, char c) {
      switch (c) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      default:
        out += c;
      }
    }

    std::size_t leadingWhitespace(const std::string& line) {
      std::size_t n = 0;
      while (n < line.size() && (line[n] == ' ' || line[n] == '\t')) {
        ++n;
      }
      return n;
    }

  } // namespace

  // ── Caché de resaltado ───────────────────────────────────────────────────────────────────────────────

  void HighlightCache::setLanguage(Language language) {
    if (language != m_language) {
      m_language = language;
      m_statesValid = 0;
      std::ranges::fill(m_spansValid, 0);
    }
  }

  void HighlightCache::sync(TextBuffer& buffer) {
    const std::size_t dirty = buffer.takeDirtyFrom();
    const std::size_t lines = buffer.lineCount();
    if (m_states.size() != lines) {
      m_states.resize(lines, 0);
      m_spans.resize(lines);
      m_spansValid.resize(lines, 0);
    }
    if (dirty < lines || dirty == 0) {
      // Las líneas desde la cambiada pueden haberse corrido (insertar/borrar líneas): todo lo de abajo se recalcula.
      const std::size_t from = std::min(dirty, lines);
      m_statesValid = std::min(m_statesValid, from);
      std::fill(m_spansValid.begin() + static_cast<std::ptrdiff_t>(from), m_spansValid.end(), 0);
    }
  }

  const std::vector<Span>& HighlightCache::spans(const TextBuffer& buffer, std::size_t line) {
    // Estados de entrada hasta `line` (se avanza desde el último válido).
    if (m_statesValid == 0 && !m_states.empty()) {
      m_states[0] = 0;
      m_statesValid = 1;
    }
    std::vector<Span> scratch;
    while (m_statesValid <= line && m_statesValid < m_states.size()) {
      const std::size_t prev = m_statesValid - 1;
      std::vector<Span>& target = m_spans[prev];
      const LineState next = highlightLine(m_language, buffer.line(prev), m_states[prev], target);
      m_spansValid[prev] = 1;
      m_states[m_statesValid++] = next;
    }
    if (m_spansValid[line] == 0) {
      (void)highlightLine(m_language, buffer.line(line), m_states[line], m_spans[line]);
      m_spansValid[line] = 1;
    }
    return m_spans[line];
  }

  // ── Vista ────────────────────────────────────────────────────────────────────────────────────────────

  EditorView::EditorView() {
    setClipChildren(true);
    auto content = std::make_unique<Node>();
    content->setClipChildren(true);
    m_content = addChild(std::move(content));

    auto currentLine = std::make_unique<Box>();
    currentLine->setFill(colorSpecFromRole(ColorRole::OnSurface, 0.05F));
    m_currentLine = static_cast<Box*>(m_content->addChild(std::move(currentLine)));

    auto input = std::make_unique<InputArea>();
    input->setFocusable(true);
    input->setCapturesAllKeys(true); // Tab y Esc son del editor, no de la navegación
    input->setRetainsFocusOnPointerRelease(true);
    input->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT);
    input->setOnKeyDown([this](const InputArea::KeyData& key) {
      if (!key.preedit) {
        onKey(key.sym, key.utf32, key.modifiers);
      }
    });
    input->setOnPress([this](const InputArea::PointerData& data) {
      if (data.pressed) {
        onPointer(data.localX, data.localY, true, false, data.time);
      } else {
        m_dragging = false;
      }
    });
    input->setOnMotion([this](const InputArea::PointerData& data) {
      if (m_dragging && m_input != nullptr && m_input->pressed()) {
        onPointer(data.localX, data.localY, false, true, data.time);
      }
    });
    input->setOnClick([this](const InputArea::PointerData&) { m_dragging = false; });
    input->setOnAxisHandler([this](const InputArea::PointerData& data) -> bool {
      if (data.axis == 0 && m_onZoom && (apps::keyboardModifiers() & KeyMod::Ctrl) != 0) {
        m_onZoom(data.axisValue < 0 ? 1 : -1);
        return true;
      }
      if (data.axis == 0) {
        m_scrollY += static_cast<float>(data.axisValue) * (data.axisSource == 1 ? 1.0F : 1.5F);
      } else if (!m_options.wrap) {
        m_scrollX += static_cast<float>(data.axisValue);
      }
      clampScroll();
      markLayoutDirty();
      return true;
    });
    input->setOnFocusGain([this] {
      m_focused = true;
      markLayoutDirty();
    });
    input->setOnFocusLoss([this] {
      m_focused = false;
      markLayoutDirty();
    });
    m_input = static_cast<InputArea*>(addChild(std::move(input)));

    auto caret = std::make_unique<Box>();
    caret->setFill(colorSpecFromRole(ColorRole::Primary));
    m_caret = static_cast<Box*>(m_content->addChild(std::move(caret)));

    auto thumb = std::make_unique<Box>();
    thumb->setFill(colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.35F));
    thumb->setRadius(3.0F);
    m_scrollThumb = static_cast<Box*>(addChild(std::move(thumb)));
  }

  EditorView::~EditorView() = default;

  void EditorView::setBuffer(TextBuffer* buffer, HighlightCache* highlight) {
    m_buffer = buffer;
    m_highlight = highlight;
    m_cursor = m_anchor = {};
    m_rowsVersion = static_cast<std::uint64_t>(-1);
    markLayoutDirty();
  }

  void EditorView::setOptions(const ViewOptions& options) {
    if (options != m_options) {
      m_options = options;
      m_rowsVersion = static_cast<std::uint64_t>(-1);
      markLayoutDirty();
    }
  }

  void EditorView::setSize(float width, float height) {
    Node::setSize(width, height);
    m_input->setSize(width, height);
    markLayoutDirty();
  }

  void EditorView::setScrollY(float scrollY) {
    m_scrollY = scrollY;
    markLayoutDirty();
  }

  // ── Geometría ──────────────────────────────────────────────────────────────────────────────────────

  void EditorView::measure(Renderer& renderer) {
    if (m_measuredSize == m_options.fontSize) {
      return;
    }
    const TextFont font{.family = kFontFamily};
    const auto wide = renderer.measureText(std::string(40, 'M'), m_options.fontSize, font);
    m_charWidth = std::max(1.0F, wide.width / 40.0F);
    m_lineHeight = std::round(m_options.fontSize * 1.55F);
    m_measuredSize = m_options.fontSize;
    m_rowsVersion = static_cast<std::uint64_t>(-1);
  }

  float EditorView::gutterWidth() const {
    if (!m_options.lineNumbers || m_buffer == nullptr) {
      return 0.0F;
    }
    const auto digits = std::to_string(m_buffer->lineCount()).size();
    return static_cast<float>(std::max<std::size_t>(digits, 3)) * m_charWidth + 2.0F * kGutterPadding;
  }

  float EditorView::textLeft() const { return gutterWidth() + kTextPadding; }

  std::size_t EditorView::visibleRows() const {
    return static_cast<std::size_t>(std::ceil(height() / m_lineHeight)) + 1;
  }

  std::size_t EditorView::cellsBefore(std::size_t line, std::size_t byte) const {
    const std::string& text = m_buffer->line(line);
    std::size_t cells = 0;
    std::size_t at = 0;
    while (at < byte && at < text.size()) {
      const char32_t c = decodeUtf8(text, at);
      cells += c == U'\t'
          ? static_cast<std::size_t>(m_options.tabWidth) - cells % static_cast<std::size_t>(m_options.tabWidth)
          : static_cast<std::size_t>(cellWidth(c));
    }
    return cells;
  }

  std::size_t EditorView::byteAtCells(std::size_t line, std::size_t from, std::size_t to, std::size_t cells) const {
    // Byte dentro de [from, to) cuya celda queda más cerca de `cells` (medida desde `from`).
    const std::string& text = m_buffer->line(line);
    const std::size_t base = cellsBefore(line, from);
    std::size_t at = from;
    std::size_t current = 0;
    while (at < to) {
      std::size_t next = at;
      const char32_t c = decodeUtf8(text, next);
      const std::size_t w = c == U'\t' ? static_cast<std::size_t>(m_options.tabWidth)
              - (base + current) % static_cast<std::size_t>(m_options.tabWidth)
                                       : static_cast<std::size_t>(cellWidth(c));
      if (current + w / 2 + (w % 2) > cells) {
        break;
      }
      current += w;
      at = next;
    }
    return at;
  }

  void EditorView::rebuildRows() {
    const float textWidth = width() - textLeft() - kTextPadding;
    if (m_rowsVersion == m_buffer->version() && (!m_options.wrap || m_rowsWidth == textWidth)) {
      return;
    }
    m_rows.clear();
    m_rows.reserve(m_buffer->lineCount());
    const auto columns = static_cast<std::size_t>(std::max(10.0F, textWidth / m_charWidth));
    for (std::size_t line = 0; line < m_buffer->lineCount(); ++line) {
      const std::string& text = m_buffer->line(line);
      if (!m_options.wrap || text.size() <= columns / 4 || cellsBefore(line, text.size()) <= columns) {
        m_rows.push_back({line, 0, text.size()});
        continue;
      }
      // Ajuste de línea: corta en el último espacio que entra; si no hay, donde se llena la fila.
      std::size_t start = 0;
      while (start < text.size()) {
        std::size_t end = byteAtCells(line, start, text.size(), columns);
        if (end >= text.size()) {
          end = text.size();
        } else {
          const auto space = text.rfind(' ', end);
          if (space != std::string::npos && space > start) {
            end = space + 1;
          }
        }
        end = std::max(end, start + 1);
        m_rows.push_back({line, start, end});
        start = end;
      }
    }
    m_rowsVersion = m_buffer->version();
    m_rowsWidth = textWidth;
  }

  std::size_t EditorView::rowOf(TextPos pos) const {
    // Búsqueda binaria: la última fila de esa línea que empieza antes del byte.
    auto it = std::upper_bound(m_rows.begin(), m_rows.end(), pos, [](TextPos a, const VisualRow& row) {
      return a < TextPos{row.line, row.start};
    });
    if (it != m_rows.begin()) {
      --it;
    }
    // Al final exacto de una fila partida, el cursor va al principio de la siguiente salvo que sea el fin de línea.
    return static_cast<std::size_t>(it - m_rows.begin());
  }

  TextPos EditorView::posAt(float x, float y) const {
    if (m_rows.empty()) {
      return {};
    }
    const auto rowIndex = static_cast<std::size_t>(
        std::clamp((y + m_scrollY) / m_lineHeight, 0.0F, static_cast<float>(m_rows.size() - 1))
    );
    const VisualRow& row = m_rows[rowIndex];
    const float cellsF = (x - textLeft() + m_scrollX) / m_charWidth;
    const auto cells = static_cast<std::size_t>(std::max(0.0F, std::round(cellsF)));
    std::size_t byte = byteAtCells(row.line, row.start, row.end, cells);
    // En una fila partida que no es la última, el fin de fila es el comienzo de la siguiente.
    if (byte == row.end && row.end < m_buffer->line(row.line).size() && byte > row.start) {
      byte = m_buffer->prevChar({row.line, byte}).col;
    }
    return {row.line, byte};
  }

  void EditorView::clampScroll() {
    const float content = static_cast<float>(m_rows.size()) * m_lineHeight;
    // Se puede bajar hasta que la última línea quede arriba de la mitad, como en GNOME.
    const float maxY = std::max(0.0F, content - height() * 0.5F);
    m_scrollY = std::clamp(m_scrollY, 0.0F, maxY);
    if (m_options.wrap) {
      m_scrollX = 0.0F;
    }
    m_scrollX = std::max(0.0F, m_scrollX);
  }

  void EditorView::scrollToCursor(bool center) {
    if (m_buffer == nullptr || m_rows.empty()) {
      return;
    }
    const float top = static_cast<float>(rowOf(m_cursor)) * m_lineHeight;
    if (center && (top < m_scrollY || top + m_lineHeight > m_scrollY + height())) {
      m_scrollY = top - height() * 0.4F;
    } else if (top < m_scrollY) {
      m_scrollY = top;
    } else if (top + m_lineHeight > m_scrollY + height()) {
      m_scrollY = top + m_lineHeight - height();
    }
    if (!m_options.wrap) {
      const float x = static_cast<float>(cellsBefore(m_cursor.line, m_cursor.col)) * m_charWidth;
      const float visible = width() - textLeft() - kTextPadding;
      if (x < m_scrollX) {
        m_scrollX = std::max(0.0F, x - visible * 0.25F);
      } else if (x > m_scrollX + visible - m_charWidth) {
        m_scrollX = x - visible * 0.75F;
      }
    }
    clampScroll();
    markLayoutDirty();
  }

  std::size_t EditorView::topLine() const {
    if (m_rows.empty()) {
      return 0;
    }
    const auto row = static_cast<std::size_t>(m_scrollY / m_lineHeight);
    return m_rows[std::min(row, m_rows.size() - 1)].line;
  }

  void EditorView::setTopLine(std::size_t line) {
    m_scrollY = static_cast<float>(line) * m_lineHeight; // aproximado con ajuste; se corrige al acomodar
    markLayoutDirty();
  }

  // ── Dibujo ─────────────────────────────────────────────────────────────────────────────────────────

  std::string EditorView::markupFor(const VisualRow& row) {
    const std::string& text = m_buffer->line(row.line);
    const std::vector<Span>& spans = m_highlight->spans(*m_buffer, row.line);
    std::string out;
    out.reserve((row.end - row.start) * 2);
    std::size_t cells = cellsBefore(row.line, row.start);
    std::size_t spanIndex = 0;
    bool open = false;
    std::size_t openEnd = 0;
    std::size_t at = row.start;
    while (at < row.end) {
      if (open && at >= openEnd) {
        out += "</span>";
        open = false;
      }
      while (spanIndex < spans.size() && spans[spanIndex].start + spans[spanIndex].length <= at) {
        ++spanIndex;
      }
      if (!open && spanIndex < spans.size() && spans[spanIndex].start <= at) {
        const TokenStyle style = styleFor(spans[spanIndex].token);
        out += std::format(
            "<span foreground=\"{}\"{}{}>", formatRgbHex(tokenColor(style)), style.bold ? " weight=\"bold\"" : "",
            style.italic ? " style=\"italic\"" : ""
        );
        open = true;
        openEnd = spans[spanIndex].start + spans[spanIndex].length;
      }
      std::size_t next = at;
      const char32_t c = decodeUtf8(text, next);
      if (c == U'\t') {
        const std::size_t width =
            static_cast<std::size_t>(m_options.tabWidth) - cells % static_cast<std::size_t>(m_options.tabWidth);
        out.append(width, ' ');
        cells += width;
      } else {
        for (std::size_t k = at; k < next; ++k) {
          appendEscaped(out, text[k]);
        }
        cells += static_cast<std::size_t>(cellWidth(c));
      }
      at = next;
    }
    if (open) {
      out += "</span>";
    }
    return out;
  }

  void EditorView::doLayout(Renderer& renderer) {
    m_renderer = &renderer;
    if (m_buffer == nullptr || m_highlight == nullptr) {
      return;
    }
    measure(renderer);
    m_highlight->sync(*m_buffer);
    rebuildRows();
    clampScroll();
    refresh();
    Node::doLayout(renderer);
  }

  void EditorView::refresh() {
    const float w = width();
    const float h = height();
    m_content->setSize(w, h);
    m_input->setSize(w, h);
    const float gutter = gutterWidth();
    const float left = textLeft();
    const std::size_t firstRow = static_cast<std::size_t>(m_scrollY / m_lineHeight);
    const std::size_t count = std::min(visibleRows(), m_rows.size() > firstRow ? m_rows.size() - firstRow : 0);
    const float offset = -std::fmod(m_scrollY, m_lineHeight);

    // Filas de texto y números de línea: se reutilizan los Label entre cuadros.
    const auto ensureLabels = [&](std::vector<Label*>& labels, std::size_t n) {
      while (labels.size() < n) {
        auto label = std::make_unique<Label>();
        label->setFontFamily(kFontFamily);
        label->setHitTestVisible(false);
        label->setMaxLines(1);
        labels.push_back(static_cast<Label*>(m_content->addChild(std::move(label))));
      }
      for (std::size_t i = 0; i < labels.size(); ++i) {
        labels[i]->setVisible(i < n);
      }
    };
    ensureLabels(m_lineLabels, count);
    ensureLabels(m_numberLabels, m_options.lineNumbers ? count : 0);

    const std::size_t cursorRow = rowOf(m_cursor);
    for (std::size_t i = 0; i < count; ++i) {
      const VisualRow& row = m_rows[firstRow + i];
      const float y = offset + static_cast<float>(i) * m_lineHeight;
      Label* label = m_lineLabels[i];
      label->setFontSize(m_options.fontSize);
      label->setUseMarkup(true);
      label->setColor(colorSpecFromRole(ColorRole::OnSurface));
      label->setText(markupFor(row));
      label->setPosition(left - m_scrollX, y + (m_lineHeight - m_options.fontSize * 1.3F) * 0.5F);
      if (m_options.lineNumbers) {
        Label* number = m_numberLabels[i];
        const bool first = row.start == 0;
        number->setFontSize(m_options.fontSize);
        number->setText(first ? std::to_string(row.line + 1) : std::string());
        number->setColor(colorSpecFromRole(
            row.line == m_cursor.line ? ColorRole::OnSurface : ColorRole::OnSurfaceVariant,
            row.line == m_cursor.line ? 1.0F : 0.6F
        ));
        const auto digits = static_cast<float>(std::to_string(row.line + 1).size());
        number->setPosition(
            gutter - kGutterPadding - digits * m_charWidth, y + (m_lineHeight - m_options.fontSize * 1.3F) * 0.5F
        );
      }
    }

    // Línea actual.
    const bool showCurrent = m_options.highlightLine && cursorRow >= firstRow && cursorRow < firstRow + count;
    m_currentLine->setVisible(showCurrent);
    if (showCurrent) {
      m_currentLine->setPosition(0.0F, offset + static_cast<float>(cursorRow - firstRow) * m_lineHeight);
      m_currentLine->setSize(w, m_lineHeight);
    }

    // Selección, coincidencias de búsqueda y línea con error: rectángulos por fila visible.
    std::size_t used = 0;
    const auto rect = [&](float x, float y, float rw, float rh, const ColorSpec& color) {
      if (used >= kMaxHighlights) {
        return;
      }
      if (used == m_highlights.size()) {
        auto box = std::make_unique<Box>();
        box->setHitTestVisible(false);
        // Debajo del texto: se insertan antes de las etiquetas.
        m_highlights.push_back(static_cast<Box*>(m_content->insertChildAt(1, std::move(box))));
      }
      Box* box = m_highlights[used++];
      box->setVisible(true);
      box->setFill(color);
      box->setPosition(x, y);
      box->setSize(std::max(rw, 2.0F), rh);
    };
    const auto rangeRects = [&](TextRange range, const ColorSpec& color) {
      range = range.ordered();
      for (std::size_t i = 0; i < count; ++i) {
        const VisualRow& row = m_rows[firstRow + i];
        const TextPos rowFrom{row.line, row.start};
        const TextPos rowTo{row.line, row.end};
        if (range.to < rowFrom || rowTo < range.from) {
          continue;
        }
        const std::size_t from = range.from <= rowFrom ? row.start : range.from.col;
        std::size_t to = rowTo <= range.to ? row.end : range.to.col;
        float extra = 0.0F;
        if (rowTo < range.to && row.end == m_buffer->line(row.line).size()) {
          extra = m_charWidth * 0.5F; // el salto de línea seleccionado se ve
        }
        to = std::max(to, from);
        const float x0 = left
            - m_scrollX
            + static_cast<float>(cellsBefore(row.line, from) - cellsBefore(row.line, row.start)) * m_charWidth;
        const float x1 = left
            - m_scrollX
            + static_cast<float>(cellsBefore(row.line, to) - cellsBefore(row.line, row.start)) * m_charWidth;
        rect(x0, offset + static_cast<float>(i) * m_lineHeight, x1 - x0 + extra, m_lineHeight, color);
      }
    };
    if (m_problem.has_value()) {
      const TextRange line{
          {m_problem->line, 0},
          {m_problem->line, m_buffer->line(std::min(m_problem->line, m_buffer->lineCount() - 1)).size()}
      };
      rangeRects(line, colorSpecFromRole(ColorRole::Error, 0.14F));
    }
    for (const auto& match : m_matches) {
      if (match.to.line + 1 >= m_rows[firstRow].line
          && match.from.line <= m_rows[std::min(firstRow + count, m_rows.size()) - 1].line + 1) {
        rangeRects(match, colorSpecFromRole(ColorRole::Tertiary, 0.30F));
      }
    }
    if (m_cursor != m_anchor) {
      rangeRects({m_anchor, m_cursor}, colorSpecFromRole(ColorRole::Primary, m_focused ? 0.32F : 0.18F));
    }
    for (std::size_t i = used; i < m_highlights.size(); ++i) {
      m_highlights[i]->setVisible(false);
    }

    // Cursor.
    const bool caretVisible = m_focused && cursorRow >= firstRow && cursorRow < firstRow + count;
    m_caret->setVisible(caretVisible);
    if (caretVisible) {
      const VisualRow& row = m_rows[cursorRow];
      const float x = left
          - m_scrollX
          + static_cast<float>(cellsBefore(row.line, m_cursor.col) - cellsBefore(row.line, row.start)) * m_charWidth;
      m_caret->setPosition(
          std::round(x) - 1.0F, offset + static_cast<float>(cursorRow - firstRow) * m_lineHeight + 2.0F
      );
      m_caret->setSize(2.0F, m_lineHeight - 4.0F);
    }

    // Barra de desplazamiento (solo indicador).
    const float content = static_cast<float>(m_rows.size()) * m_lineHeight + h * 0.5F;
    const bool scrollable = content > h + 1.0F;
    m_scrollThumb->setVisible(scrollable);
    if (scrollable) {
      const float thumb = std::max(24.0F, h * h / content);
      m_scrollThumb->setPosition(w - 8.0F, (h - thumb) * (m_scrollY / std::max(1.0F, content - h)));
      m_scrollThumb->setSize(5.0F, thumb);
    }
    markPaintDirty();
  }

  // ── Selección y movimiento ─────────────────────────────────────────────────────────────────────────

  std::string EditorView::selectedText() const {
    return m_buffer == nullptr ? std::string() : m_buffer->textIn({m_anchor, m_cursor});
  }

  void EditorView::moveTo(TextPos pos, bool extend, bool keepColumn) {
    m_cursor = m_buffer->clamp(pos);
    if (!extend) {
      m_anchor = m_cursor;
    }
    if (!keepColumn) {
      m_preferredCells.reset();
    }
    m_buffer->breakUndoGroup();
    scrollToCursor();
    if (m_onCursorMoved) {
      m_onCursorMoved();
    }
  }

  void EditorView::setCursor(TextPos pos, bool extend) {
    if (m_buffer != nullptr) {
      moveTo(pos, extend);
    }
  }

  void EditorView::select(TextRange range) {
    if (m_buffer == nullptr) {
      return;
    }
    m_anchor = m_buffer->clamp(range.from);
    m_cursor = m_buffer->clamp(range.to);
    m_buffer->breakUndoGroup();
    scrollToCursor(true);
    if (m_onCursorMoved) {
      m_onCursorMoved();
    }
  }

  void EditorView::selectAll() {
    if (m_buffer != nullptr) {
      m_anchor = {0, 0};
      m_cursor = m_buffer->end();
      markLayoutDirty();
      if (m_onCursorMoved) {
        m_onCursorMoved();
      }
    }
  }

  void EditorView::setSearchMatches(std::vector<TextRange> matches) {
    m_matches = std::move(matches);
    markLayoutDirty();
  }

  void EditorView::setProblem(std::optional<TextPos> at) {
    m_problem = at;
    markLayoutDirty();
  }

  void EditorView::moveVertical(int rows, bool extend) {
    if (m_rows.empty()) {
      return;
    }
    const std::size_t row = rowOf(m_cursor);
    const VisualRow& current = m_rows[row];
    if (!m_preferredCells) {
      m_preferredCells = cellsBefore(m_cursor.line, m_cursor.col) - cellsBefore(current.line, current.start);
    }
    const auto target = static_cast<std::size_t>(
        std::clamp<long>(static_cast<long>(row) + rows, 0, static_cast<long>(m_rows.size()) - 1)
    );
    if (target == row) {
      moveTo(rows < 0 ? TextPos{0, 0} : m_buffer->end(), extend);
      return;
    }
    const VisualRow& dest = m_rows[target];
    std::size_t byte = byteAtCells(dest.line, dest.start, dest.end, *m_preferredCells);
    if (byte == dest.end && dest.end < m_buffer->line(dest.line).size() && byte > dest.start) {
      byte = m_buffer->prevChar({dest.line, byte}).col;
    }
    moveTo({dest.line, byte}, extend, true);
  }

  void EditorView::onPointer(float x, float y, bool press, bool extend, std::uint32_t time) {
    if (m_buffer == nullptr) {
      return;
    }
    const TextPos pos = posAt(x, y);
    if (press) {
      m_clickCount = time - m_lastClick < kMultiClickMs ? m_clickCount % 3 + 1 : 1;
      m_lastClick = time;
      m_dragging = true;
      const bool shift = (apps::keyboardModifiers() & KeyMod::Shift) != 0;
      if (m_clickCount == 2) {
        const TextRange word = m_buffer->wordAt(pos);
        m_anchor = word.from;
        m_cursor = word.to;
      } else if (m_clickCount == 3) {
        m_anchor = {pos.line, 0};
        m_cursor = pos.line + 1 < m_buffer->lineCount() ? TextPos{pos.line + 1, 0}
                                                        : TextPos{pos.line, m_buffer->line(pos.line).size()};
      } else {
        moveTo(pos, shift);
        return;
      }
      m_buffer->breakUndoGroup();
      markLayoutDirty();
      if (m_onCursorMoved) {
        m_onCursorMoved();
      }
      return;
    }
    if (extend && m_clickCount <= 1) {
      moveTo(pos, true);
    }
  }

  // ── Edición ────────────────────────────────────────────────────────────────────────────────────────

  void EditorView::edited() {
    m_rowsVersion = static_cast<std::uint64_t>(-1);
    m_preferredCells.reset();
    scrollToCursor();
    if (m_onChanged) {
      m_onChanged();
    }
    if (m_onCursorMoved) {
      m_onCursorMoved();
    }
  }

  void EditorView::replaceSelection(std::string_view text) {
    const TextRange range = TextRange{m_anchor, m_cursor}.ordered();
    m_cursor = range.empty() ? m_buffer->insert(range.from, text) : m_buffer->replace(range, text);
    m_anchor = m_cursor;
    edited();
  }

  void EditorView::insertText(std::string_view text) {
    if (m_buffer != nullptr) {
      replaceSelection(text);
    }
  }

  void EditorView::newline() {
    // Sangría automática: la de la línea actual, un nivel más tras { [ ( o una etiqueta abierta.
    const std::string& line = m_buffer->line(m_cursor.line);
    std::string indent = line.substr(0, std::min(leadingWhitespace(line), m_cursor.col));
    const std::string unit =
        m_options.insertSpaces ? std::string(static_cast<std::size_t>(m_options.tabWidth), ' ') : std::string("\t");
    std::size_t before = m_cursor.col;
    while (before > 0 && (line[before - 1] == ' ' || line[before - 1] == '\t')) {
      --before;
    }
    const char last = before > 0 ? line[before - 1] : '\0';
    const char next = m_cursor.col < line.size() ? line[m_cursor.col] : '\0';
    const bool opens = last == '{' || last == '[' || last == '(';
    if (opens && ((last == '{' && next == '}') || (last == '[' && next == ']') || (last == '(' && next == ')'))) {
      // Entre llaves: el cierre baja a su propia línea y el cursor queda en medio.
      replaceSelection("\n" + indent + unit + "\n" + indent);
      m_cursor = m_anchor = {m_cursor.line - 1, indent.size() + unit.size()};
      edited();
      return;
    }
    replaceSelection("\n" + indent + (opens ? unit : std::string()));
  }

  void EditorView::indentLines(bool outdent) {
    const TextRange range = TextRange{m_anchor, m_cursor}.ordered();
    const std::string unit =
        m_options.insertSpaces ? std::string(static_cast<std::size_t>(m_options.tabWidth), ' ') : std::string("\t");
    if (!outdent && range.empty()) {
      // Tab sin selección: hasta la próxima parada de tabulación.
      if (m_options.insertSpaces) {
        const std::size_t cells = cellsBefore(m_cursor.line, m_cursor.col);
        replaceSelection(
            std::string(
                static_cast<std::size_t>(m_options.tabWidth) - cells % static_cast<std::size_t>(m_options.tabWidth), ' '
            )
        );
      } else {
        replaceSelection("\t");
      }
      return;
    }
    std::size_t last = range.to.line;
    if (range.to.col == 0 && range.to.line > range.from.line) {
      --last; // una selección que termina al comienzo de una línea no la incluye
    }
    m_buffer->breakUndoGroup();
    for (std::size_t line = range.from.line; line <= last; ++line) {
      if (outdent) {
        const std::string& text = m_buffer->line(line);
        std::size_t remove = 0;
        if (!text.empty() && text[0] == '\t') {
          remove = 1;
        } else {
          while (remove < text.size() && remove < static_cast<std::size_t>(m_options.tabWidth) && text[remove] == ' ') {
            ++remove;
          }
        }
        m_buffer->erase({{line, 0}, {line, remove}});
      } else if (!m_buffer->line(line).empty()) {
        (void)m_buffer->insert({line, 0}, unit);
        m_buffer->breakUndoGroup();
      }
    }
    m_anchor = {range.from.line, 0};
    m_cursor = {last, m_buffer->line(last).size()};
    edited();
  }

  void EditorView::toggleComment() {
    const std::string_view prefix = lineComment(m_highlight->language());
    if (prefix.empty()) {
      return;
    }
    const TextRange range = TextRange{m_anchor, m_cursor}.ordered();
    std::size_t last = range.to.line;
    if (range.to.col == 0 && range.to.line > range.from.line) {
      --last;
    }
    // Si todas las líneas con texto ya están comentadas, se descomenta; si no, se comenta.
    bool allCommented = true;
    for (std::size_t line = range.from.line; line <= last; ++line) {
      const std::string& text = m_buffer->line(line);
      const std::size_t lead = leadingWhitespace(text);
      if (lead < text.size() && !std::string_view(text).substr(lead).starts_with(prefix)) {
        allCommented = false;
      }
    }
    m_buffer->breakUndoGroup();
    for (std::size_t line = range.from.line; line <= last; ++line) {
      const std::string& text = m_buffer->line(line);
      const std::size_t lead = leadingWhitespace(text);
      if (lead == text.size()) {
        continue;
      }
      if (allCommented) {
        std::size_t length = prefix.size();
        if (lead + length < text.size() && text[lead + length] == ' ') {
          ++length;
        }
        m_buffer->erase({{line, lead}, {line, lead + length}});
      } else {
        (void)m_buffer->insert({line, lead}, std::string(prefix) + " ");
        m_buffer->breakUndoGroup();
      }
    }
    m_anchor = {range.from.line, 0};
    m_cursor = {last, m_buffer->line(last).size()};
    edited();
  }

  void EditorView::moveLines(int direction) {
    const TextRange range = TextRange{m_anchor, m_cursor}.ordered();
    std::size_t first = range.from.line;
    std::size_t last = range.to.line;
    if (range.to.col == 0 && last > first) {
      --last;
    }
    if ((direction < 0 && first == 0) || (direction > 0 && last + 1 >= m_buffer->lineCount())) {
      return;
    }
    // Se mueve la línea vecina al otro lado del bloque: un solo paso de deshacer.
    m_buffer->breakUndoGroup();
    if (direction < 0) {
      const std::string above = m_buffer->line(first - 1);
      m_buffer->erase({{first - 1, 0}, {first, 0}});
      (void)m_buffer->insert({last - 1, m_buffer->line(last - 1).size()}, "\n" + above);
    } else {
      const std::string below = m_buffer->line(last + 1);
      m_buffer->erase({{last, m_buffer->line(last).size()}, {last + 1, below.size()}});
      (void)m_buffer->insert({first, 0}, below + "\n");
    }
    const auto shift = [direction](TextPos pos) {
      return TextPos{pos.line + static_cast<std::size_t>(direction), pos.col};
    };
    m_anchor = shift(m_anchor);
    m_cursor = shift(m_cursor);
    edited();
  }

  void EditorView::deleteLines() {
    const TextRange range = TextRange{m_anchor, m_cursor}.ordered();
    const std::size_t first = range.from.line;
    const std::size_t last = range.to.line;
    if (last + 1 < m_buffer->lineCount()) {
      m_buffer->erase({{first, 0}, {last + 1, 0}});
    } else if (first > 0) {
      m_buffer->erase({{first - 1, m_buffer->line(first - 1).size()}, {last, m_buffer->line(last).size()}});
    } else {
      m_buffer->erase({{0, 0}, m_buffer->end()});
    }
    m_cursor = m_anchor = m_buffer->clamp({first, 0});
    edited();
  }

  void EditorView::duplicateLines() {
    const TextRange range = TextRange{m_anchor, m_cursor}.ordered();
    if (!range.empty()) {
      const std::string text = m_buffer->textIn(range);
      m_cursor = m_buffer->insert(range.to, text);
      m_anchor = range.to;
      edited();
      return;
    }
    const std::string line = m_buffer->line(m_cursor.line);
    (void)m_buffer->insert({m_cursor.line, line.size()}, "\n" + line);
    m_cursor = m_anchor = {m_cursor.line + 1, m_cursor.col};
    edited();
  }

  void EditorView::backspace(bool word) {
    if (m_cursor != m_anchor) {
      replaceSelection("");
      return;
    }
    TextPos from = word ? m_buffer->prevWord(m_cursor) : m_buffer->prevChar(m_cursor);
    // En la sangría con espacios, Retroceso quita hasta la parada anterior.
    const std::string& line = m_buffer->line(m_cursor.line);
    if (!word
        && m_options.insertSpaces
        && m_cursor.col > 0
        && leadingWhitespace(line) >= m_cursor.col
        && line.substr(0, m_cursor.col).find('\t') == std::string::npos) {
      const std::size_t tab = static_cast<std::size_t>(m_options.tabWidth);
      from = {m_cursor.line, (m_cursor.col - 1) / tab * tab};
    }
    m_buffer->erase({from, m_cursor});
    m_cursor = m_anchor = from;
    edited();
  }

  void EditorView::deleteForward(bool word) {
    if (m_cursor != m_anchor) {
      replaceSelection("");
      return;
    }
    const TextPos to = word ? m_buffer->nextWord(m_cursor) : m_buffer->nextChar(m_cursor);
    m_buffer->erase({m_cursor, to});
    edited();
  }

  void EditorView::undo() {
    if (m_buffer == nullptr) {
      return;
    }
    if (const auto pos = m_buffer->undo()) {
      m_cursor = m_anchor = *pos;
      edited();
    }
  }

  void EditorView::redo() {
    if (m_buffer == nullptr) {
      return;
    }
    if (const auto pos = m_buffer->redo()) {
      m_cursor = m_anchor = *pos;
      edited();
    }
  }

  void EditorView::copy() {
    if (TextClipboard* clipboard = Input::textClipboard(); clipboard != nullptr && m_cursor != m_anchor) {
      clipboard->setClipboardText(selectedText());
    }
  }

  void EditorView::cut() {
    if (m_cursor != m_anchor) {
      copy();
      replaceSelection("");
    }
  }

  void EditorView::paste() {
    if (TextClipboard* clipboard = Input::textClipboard(); clipboard != nullptr) {
      if (const auto text = clipboard->clipboardText(); text.has_value() && !text->empty()) {
        m_buffer->breakUndoGroup();
        replaceSelection(*text);
        m_buffer->breakUndoGroup();
      }
    }
  }

  void EditorView::onKey(std::uint32_t sym, std::uint32_t utf32, std::uint32_t modifiers) {
    if (m_buffer == nullptr) {
      return;
    }
    const bool ctrl = (modifiers & KeyMod::Ctrl) != 0;
    const bool shift = (modifiers & KeyMod::Shift) != 0;
    const bool alt = (modifiers & KeyMod::Alt) != 0;
    const std::size_t page = std::max<std::size_t>(1, static_cast<std::size_t>(height() / m_lineHeight) - 1);
    switch (sym) {
    case XKB_KEY_Left:
      if (!shift && m_cursor != m_anchor) {
        moveTo(TextRange{m_anchor, m_cursor}.ordered().from, false);
      } else {
        moveTo(ctrl ? m_buffer->prevWord(m_cursor) : m_buffer->prevChar(m_cursor), shift);
      }
      break;
    case XKB_KEY_Right:
      if (!shift && m_cursor != m_anchor) {
        moveTo(TextRange{m_anchor, m_cursor}.ordered().to, false);
      } else {
        moveTo(ctrl ? m_buffer->nextWord(m_cursor) : m_buffer->nextChar(m_cursor), shift);
      }
      break;
    case XKB_KEY_Up:
      if (alt) {
        moveLines(-1);
      } else {
        moveVertical(-1, shift);
      }
      break;
    case XKB_KEY_Down:
      if (alt) {
        moveLines(1);
      } else {
        moveVertical(1, shift);
      }
      break;
    case XKB_KEY_Page_Up:
      moveVertical(-static_cast<int>(page), shift);
      break;
    case XKB_KEY_Page_Down:
      moveVertical(static_cast<int>(page), shift);
      break;
    case XKB_KEY_Home: {
      if (ctrl) {
        moveTo({0, 0}, shift);
        break;
      }
      // Inicio inteligente: primero al texto, luego a la columna 0.
      const std::size_t lead = leadingWhitespace(m_buffer->line(m_cursor.line));
      moveTo({m_cursor.line, m_cursor.col == lead ? 0 : lead}, shift);
      break;
    }
    case XKB_KEY_End:
      moveTo(ctrl ? m_buffer->end() : TextPos{m_cursor.line, m_buffer->line(m_cursor.line).size()}, shift);
      break;
    case XKB_KEY_BackSpace:
      backspace(ctrl);
      break;
    case XKB_KEY_Delete:
      deleteForward(ctrl);
      break;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
      newline();
      break;
    case XKB_KEY_Tab:
    case XKB_KEY_ISO_Left_Tab:
      indentLines(shift || sym == XKB_KEY_ISO_Left_Tab);
      break;
    default:
      if (ctrl) {
        switch (sym) {
        case XKB_KEY_a:
          selectAll();
          break;
        case XKB_KEY_c:
          copy();
          break;
        case XKB_KEY_x:
          cut();
          break;
        case XKB_KEY_v:
          paste();
          break;
        case XKB_KEY_z:
          undo();
          break;
        case XKB_KEY_Z:
        case XKB_KEY_y:
          redo();
          break;
        case XKB_KEY_d:
          deleteLines();
          break;
        case XKB_KEY_D:
          duplicateLines();
          break;
        case XKB_KEY_slash:
          toggleComment();
          break;
        default:
          return; // atajos de la ventana
        }
        break;
      }
      if (utf32 >= 0x20U && utf32 != 0x7FU && !alt) {
        std::string text;
        // UTF-32 → UTF-8.
        if (utf32 < 0x80) {
          text = static_cast<char>(utf32);
        } else if (utf32 < 0x800) {
          text = {static_cast<char>(0xC0 | (utf32 >> 6)), static_cast<char>(0x80 | (utf32 & 0x3F))};
        } else if (utf32 < 0x10000) {
          text = {
              static_cast<char>(0xE0 | (utf32 >> 12)), static_cast<char>(0x80 | ((utf32 >> 6) & 0x3F)),
              static_cast<char>(0x80 | (utf32 & 0x3F))
          };
        } else {
          text = {
              static_cast<char>(0xF0 | (utf32 >> 18)), static_cast<char>(0x80 | ((utf32 >> 12) & 0x3F)),
              static_cast<char>(0x80 | ((utf32 >> 6) & 0x3F)), static_cast<char>(0x80 | (utf32 & 0x3F))
          };
        }
        replaceSelection(text);
      }
      return;
    }
    markLayoutDirty();
  }

} // namespace okaru::editor
