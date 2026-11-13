#include "apps/files/file_view.h"

#include "apps/common/okaru_app.h"
#include "core/input/key_modifiers.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "ui/controls/box.h"
#include "ui/controls/glyph.h"
#include "ui/controls/image.h"
#include "ui/controls/label.h"
#include "ui/controls/scrollbar.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <glib.h>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace okaru::files {

  namespace {

    constexpr float kListIcon = 22.0F;
    constexpr float kPad = 6.0F;
    constexpr std::uint32_t kDoubleClickMs = 400;
    constexpr std::uint32_t kTypeAheadMs = 900;

    struct Columns {
      float name = 0.0F;
      float location = 0.0F;
      float size = 0.0F;
      float modified = 0.0F;
      float type = 0.0F;
    };

    Columns listColumns(float width, float scale, bool location) {
      Columns c;
      c.size = width > 380.0F * scale ? 96.0F * scale : 0.0F;
      c.modified = width > 520.0F * scale ? 150.0F * scale : 0.0F;
      c.type = width > 760.0F * scale ? 170.0F * scale : 0.0F;
      c.location = location && width > 600.0F * scale ? 220.0F * scale : 0.0F;
      c.name = std::max(80.0F * scale, width - c.size - c.modified - c.type - c.location - 2.0F * kPad * scale);
      return c;
    }

  } // namespace

  struct FileView::Slot {
    Node* root = nullptr;
    Box* background = nullptr;
    Image* icon = nullptr;
    Box* frame = nullptr;    // borde fino de las miniaturas (una hoja blanca se perdería en el fondo)
    Glyph* fallback = nullptr; // sin icono en el tema
    Glyph* chevron = nullptr;  // modo compacto: carpetas
    Label* name = nullptr;
    Label* location = nullptr;
    Label* size = nullptr;
    Label* modified = nullptr;
    Label* type = nullptr;
  };

  FileView::FileView() {
    setClipChildren(true);
    m_content = addChild(std::make_unique<Node>());
    m_header = addChild(std::make_unique<Node>());
    m_header->setZIndex(3);
    auto band = std::make_unique<Box>();
    band->setHitTestVisible(false);
    band->setZIndex(4);
    band->setVisible(false);
    m_band = static_cast<Box*>(addChild(std::move(band)));
    auto empty = std::make_unique<Label>();
    empty->setHitTestVisible(false);
    empty->setVisible(false);
    empty->setTextAlign(TextAlign::Center);
    m_empty = static_cast<Label*>(addChild(std::move(empty)));

    auto input = std::make_unique<InputArea>();
    input->setFocusable(true);
    input->setRetainsFocusOnPointerRelease(true);
    input->setAcceptedButtons(InputArea::buttonMask({BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA}));
    input->setOnPress([this](const InputArea::PointerData& d) {
      if (d.pressed) {
        onPress(d.localX, d.localY, d.sceneX, d.sceneY, d.button, d.time, d.serial);
      } else if (d.button == BTN_LEFT) {
        onRelease(d.localX, d.localY, d.sceneX, d.sceneY);
      }
    });
    input->setOnMotion([this](const InputArea::PointerData& d) { onMotion(d.localX, d.localY, d.sceneX, d.sceneY); });
    input->setOnLeave([this] {
      if (!m_pressed && m_hover) {
        m_hover.reset();
        markLayoutDirty();
      }
    });
    input->setOnAxisHandler([this](const InputArea::PointerData& d) -> bool {
      if ((apps::keyboardModifiers() & KeyMod::Ctrl) != 0) {
        if (m_onZoom) {
          m_onZoom(d.axisValue < 0 ? 1 : -1);
        }
        return true;
      }
      if (d.axis == 0) {
        scrollBy(static_cast<float>(d.axisValue) * (d.axisSource == 1 ? 1.0F : 1.6F));
      }
      return true;
    });
    input->setOnKeyDown([this](const InputArea::KeyData& key) {
      if (!key.pressed || m_entries == nullptr) {
        return;
      }
      const bool shift = (key.modifiers & KeyMod::Shift) != 0;
      const bool ctrl = (key.modifiers & KeyMod::Ctrl) != 0;
      const int cols = m_mode == ViewMode::Grid ? columns() : 1;
      const int page = std::max(1, static_cast<int>(height() / std::max(1.0F, m_mode == ViewMode::Grid ? cellHeight() : rowHeight())) - 1) * cols;
      switch (key.sym) {
      case XKB_KEY_Down:
        moveCursor(cols, shift);
        return;
      case XKB_KEY_Up:
        moveCursor(-cols, shift);
        return;
      case XKB_KEY_Right:
        if (m_mode == ViewMode::Grid) {
          moveCursor(1, shift);
        }
        return;
      case XKB_KEY_Left:
        if (m_mode == ViewMode::Grid) {
          moveCursor(-1, shift);
        }
        return;
      case XKB_KEY_Page_Down:
        moveCursor(page, shift);
        return;
      case XKB_KEY_Page_Up:
        moveCursor(-page, shift);
        return;
      case XKB_KEY_Home:
        moveCursor(-static_cast<int>(m_entries->size()), shift);
        return;
      case XKB_KEY_End:
        moveCursor(static_cast<int>(m_entries->size()), shift);
        return;
      case XKB_KEY_Return:
      case XKB_KEY_KP_Enter:
        if (m_state != nullptr && m_state->cursor && m_onActivate) {
          m_onActivate(*m_state->cursor);
        }
        return;
      default:
        break;
      }
      if (!ctrl && (key.modifiers & KeyMod::Alt) == 0 && key.utf32 >= 0x20 && key.utf32 != 0x7F) {
        typeAhead(key.utf32);
      }
    });
    m_input = static_cast<InputArea*>(addChild(std::move(input)));
    m_input->setZIndex(2);

    auto bar = std::make_unique<Scrollbar>();
    bar->setOrientation(ScrollOrientation::Vertical);
    bar->setZIndex(5);
    bar->setOnScrollChanged([this](float offset) {
      if (m_state != nullptr) {
        m_state->scrollY = offset;
        markLayoutDirty();
      }
    });
    m_vbar = static_cast<Scrollbar*>(addChild(std::move(bar)));
  }

  FileView::~FileView() = default;

  void FileView::setSize(float width, float height) {
    Node::setSize(width, height);
    markLayoutDirty();
  }

  void FileView::setEntries(const std::vector<FileEntry>* entries, ViewState* state) {
    m_entries = entries;
    m_state = state;
    if (m_state != nullptr && m_entries != nullptr) {
      if (m_state->cursor && *m_state->cursor >= m_entries->size()) {
        m_state->cursor.reset();
      }
      if (!m_state->revealUri.empty()) {
        for (std::size_t i = 0; i < m_entries->size(); ++i) {
          if ((*m_entries)[i].uri == m_state->revealUri) {
            m_state->cursor = i;
            m_state->anchor = i;
            m_state->selected = {m_state->revealUri};
            m_state->revealUri.clear();
            ensureVisible(i);
            break;
          }
        }
      }
    }
    markLayoutDirty();
  }

  // ── Geometría ──────────────────────────────────────────────────────────────────────────────────────

  float FileView::headerHeight() const { return m_mode == ViewMode::List ? 30.0F * m_scale : 0.0F; }

  float FileView::rowHeight() const { return (m_mode == ViewMode::Compact ? 30.0F : 34.0F) * m_scale; }

  int FileView::columns() const {
    const float minCell = (m_iconSize + 56.0F) * m_scale;
    return std::max(1, static_cast<int>((width() - 2.0F * kPad * m_scale - m_vbar->reservedThickness()) / minCell));
  }

  float FileView::cellWidth() const {
    return (width() - 2.0F * kPad * m_scale - m_vbar->reservedThickness()) / static_cast<float>(columns());
  }

  float FileView::cellHeight() const { return (m_iconSize + 56.0F) * m_scale; }

  float FileView::contentHeight() const {
    const std::size_t count = m_entries != nullptr ? m_entries->size() : 0;
    if (m_mode == ViewMode::Grid) {
      const auto rows = (count + static_cast<std::size_t>(columns()) - 1) / static_cast<std::size_t>(columns());
      return static_cast<float>(rows) * cellHeight() + 2.0F * kPad * m_scale;
    }
    return static_cast<float>(count) * rowHeight() + 2.0F * kPad * m_scale;
  }

  std::pair<float, float> FileView::itemSize() const {
    if (m_mode == ViewMode::Grid) {
      return {cellWidth(), cellHeight()};
    }
    return {width() - 2.0F * kPad * m_scale - m_vbar->reservedThickness(), rowHeight()};
  }

  std::pair<float, float> FileView::itemOrigin(std::size_t index) const {
    const float pad = kPad * m_scale;
    if (m_mode == ViewMode::Grid) {
      const auto cols = static_cast<std::size_t>(columns());
      return {pad + static_cast<float>(index % cols) * cellWidth(), pad + static_cast<float>(index / cols) * cellHeight()};
    }
    return {pad, pad + static_cast<float>(index) * rowHeight()};
  }

  std::optional<std::size_t> FileView::indexAt(float x, float y) const {
    if (m_entries == nullptr || m_state == nullptr || y < headerHeight()) {
      return std::nullopt;
    }
    const float cy = y - headerHeight() + m_state->scrollY - kPad * m_scale;
    const float cx = x - kPad * m_scale;
    if (cy < 0.0F || cx < 0.0F) {
      return std::nullopt;
    }
    std::size_t index = 0;
    if (m_mode == ViewMode::Grid) {
      const auto col = static_cast<std::size_t>(cx / cellWidth());
      if (col >= static_cast<std::size_t>(columns())) {
        return std::nullopt;
      }
      index = static_cast<std::size_t>(cy / cellHeight()) * static_cast<std::size_t>(columns()) + col;
    } else {
      if (cx > itemSize().first) {
        return std::nullopt;
      }
      index = static_cast<std::size_t>(cy / rowHeight());
    }
    return index < m_entries->size() ? std::optional(index) : std::nullopt;
  }

  void FileView::clampScroll() {
    if (m_state == nullptr) {
      return;
    }
    const float maxScroll = std::max(0.0F, contentHeight() - (height() - headerHeight()));
    m_state->scrollY = std::clamp(m_state->scrollY, 0.0F, maxScroll);
  }

  void FileView::scrollBy(float dy) {
    if (m_state == nullptr) {
      return;
    }
    m_state->scrollY += dy;
    clampScroll();
    markLayoutDirty();
  }

  void FileView::ensureVisible(std::size_t index) {
    if (m_state == nullptr || height() <= 0.0F) {
      return;
    }
    const auto [x, y] = itemOrigin(index);
    const float h = itemSize().second;
    const float viewport = height() - headerHeight();
    if (y < m_state->scrollY) {
      m_state->scrollY = y - kPad * m_scale;
    } else if (y + h > m_state->scrollY + viewport) {
      m_state->scrollY = y + h + kPad * m_scale - viewport;
    }
    clampScroll();
    markLayoutDirty();
  }

  // ── Selección ──────────────────────────────────────────────────────────────────────────────────────

  std::vector<std::size_t> FileView::selectedIndices() const {
    std::vector<std::size_t> out;
    if (m_entries == nullptr || m_state == nullptr) {
      return out;
    }
    for (std::size_t i = 0; i < m_entries->size(); ++i) {
      if (m_state->selected.contains((*m_entries)[i].uri)) {
        out.push_back(i);
      }
    }
    return out;
  }

  void FileView::notifySelection() {
    markLayoutDirty();
    if (m_onSelectionChanged) {
      m_onSelectionChanged();
    }
  }

  void FileView::selectAll() {
    if (m_entries == nullptr || m_state == nullptr) {
      return;
    }
    for (const auto& e : *m_entries) {
      m_state->selected.insert(e.uri);
    }
    notifySelection();
  }

  void FileView::invertSelection() {
    if (m_entries == nullptr || m_state == nullptr) {
      return;
    }
    std::set<std::string> next;
    for (const auto& e : *m_entries) {
      if (!m_state->selected.contains(e.uri)) {
        next.insert(e.uri);
      }
    }
    m_state->selected = std::move(next);
    notifySelection();
  }

  void FileView::clearSelection() {
    if (m_state == nullptr) {
      return;
    }
    m_state->selected.clear();
    notifySelection();
  }

  void FileView::selectUris(const std::vector<std::string>& uris) {
    if (m_state == nullptr) {
      return;
    }
    m_state->selected = {uris.begin(), uris.end()};
    if (m_entries != nullptr) {
      for (std::size_t i = 0; i < m_entries->size(); ++i) {
        if (!uris.empty() && (*m_entries)[i].uri == uris.front()) {
          m_state->cursor = i;
          m_state->anchor = i;
          ensureVisible(i);
        }
      }
    }
    notifySelection();
  }

  void FileView::applyPressSelection(std::size_t index, std::uint32_t modifiers) {
    const std::string& uri = (*m_entries)[index].uri;
    if ((modifiers & KeyMod::Shift) != 0 && m_state->anchor) {
      if ((modifiers & KeyMod::Ctrl) == 0) {
        m_state->selected.clear();
      }
      const std::size_t a = std::min(*m_state->anchor, index);
      const std::size_t b = std::max(*m_state->anchor, index);
      for (std::size_t i = a; i <= b && i < m_entries->size(); ++i) {
        m_state->selected.insert((*m_entries)[i].uri);
      }
    } else if ((modifiers & KeyMod::Ctrl) != 0) {
      if (!m_state->selected.erase(uri)) {
        m_state->selected.insert(uri);
      }
      m_state->anchor = index;
    } else {
      m_state->selected = {uri};
      m_state->anchor = index;
    }
    m_state->cursor = index;
    notifySelection();
  }

  void FileView::moveCursor(int delta, bool extend) {
    if (m_entries == nullptr || m_state == nullptr || m_entries->empty()) {
      return;
    }
    const auto count = static_cast<long>(m_entries->size());
    const long from = m_state->cursor ? static_cast<long>(*m_state->cursor) : (delta > 0 ? -1 : count);
    const auto to = static_cast<std::size_t>(std::clamp(from + delta, 0L, count - 1));
    applyPressSelection(to, extend ? KeyMod::Shift : 0);
    ensureVisible(to);
  }

  void FileView::typeAhead(std::uint32_t codepoint) {
    const auto now = static_cast<std::uint32_t>(g_get_monotonic_time() / 1000);
    if (now - m_typedTime > kTypeAheadMs) {
      m_typed.clear();
    }
    m_typedTime = now;
    char buffer[8];
    const int length = g_unichar_to_utf8(codepoint, buffer);
    m_typed.append(buffer, static_cast<std::size_t>(length));
    const std::string needle = normalizeForMatch(m_typed);
    // Desde el cursor: repetir la misma letra salta a la siguiente coincidencia.
    const std::size_t count = m_entries->size();
    const std::size_t start = m_state->cursor ? *m_state->cursor + (m_typed.size() == static_cast<std::size_t>(length) ? 1 : 0) : 0;
    for (std::size_t k = 0; k < count; ++k) {
      const std::size_t i = (start + k) % count;
      if (normalizeForMatch((*m_entries)[i].display).starts_with(needle)) {
        applyPressSelection(i, 0);
        ensureVisible(i);
        return;
      }
    }
  }

  // ── Puntero ────────────────────────────────────────────────────────────────────────────────────────

  void FileView::onPress(float x, float y, float sceneX, float sceneY, std::uint32_t button, std::uint32_t time,
                         std::uint32_t serial) {
    if (m_entries == nullptr || m_state == nullptr) {
      return;
    }
    if (button == BTN_SIDE || button == BTN_EXTRA) {
      if (m_onBack) {
        m_onBack(button == BTN_EXTRA);
      }
      return;
    }
    // Encabezado de la lista: ordenar por la columna.
    if (y < headerHeight()) {
      if (button != BTN_LEFT || !m_onSortChanged) {
        return;
      }
      const Columns c = listColumns(itemSize().first, m_scale, m_showLocation);
      float edge = kPad * m_scale + c.name + c.location;
      SortKey key = SortKey::Name;
      if (x > edge && c.size > 0.0F && x <= edge + c.size) {
        key = SortKey::Size;
      }
      edge += c.size;
      if (x > edge && c.modified > 0.0F && x <= edge + c.modified) {
        key = SortKey::Modified;
      }
      edge += c.modified;
      if (x > edge && c.type > 0.0F) {
        key = SortKey::Type;
      }
      m_onSortChanged(key);
      return;
    }
    const auto index = indexAt(x, y);
    const std::uint32_t modifiers = apps::keyboardModifiers();
    if (button == BTN_MIDDLE) {
      if (index && m_onMiddleClick) {
        m_onMiddleClick(*index);
      }
      return;
    }
    if (button == BTN_RIGHT) {
      if (index && !m_state->selected.contains((*m_entries)[*index].uri)) {
        applyPressSelection(*index, 0);
      } else if (!index && (modifiers & KeyMod::Ctrl) == 0) {
        clearSelection();
      }
      if (m_onContextMenu) {
        m_onContextMenu(index, sceneX, sceneY);
      }
      return;
    }

    // Doble clic: abrir.
    if (index && m_lastClickIndex == index && time - m_lastClickTime < kDoubleClickMs && modifiers == 0) {
      m_lastClickIndex.reset();
      if (m_onActivate) {
        m_onActivate(*index);
      }
      return;
    }
    m_lastClickIndex = index;
    m_lastClickTime = time;

    m_pressed = true;
    m_dragging = false;
    m_pressIndex = index;
    m_pressModifiers = modifiers;
    m_pressSerial = serial;
    m_pressX = x;
    m_pressY = y - headerHeight() + m_state->scrollY;
    m_pointerX = x;
    m_pointerY = y;
    m_pressOnSelected = index && m_state->selected.contains((*m_entries)[*index].uri);
    m_banding = !index;
    if (m_banding) {
      m_bandBase = (modifiers & (KeyMod::Ctrl | KeyMod::Shift)) != 0 ? m_state->selected : std::set<std::string>{};
      if (m_bandBase.empty() && !m_state->selected.empty()) {
        m_state->selected.clear();
        notifySelection();
      }
    } else if (!m_pressOnSelected || modifiers != 0) {
      // Clic sobre lo ya seleccionado sin teclas: se decide al soltar (puede ser el inicio de un arrastre).
      applyPressSelection(*index, modifiers);
    }
  }

  void FileView::onMotion(float x, float y, float sceneX, float sceneY) {
    m_pointerX = x;
    m_pointerY = y;
    if (!m_pressed) {
      const auto hover = indexAt(x, y);
      if (hover != m_hover) {
        m_hover = hover;
        markLayoutDirty();
      }
      return;
    }
    // Cerca de los bordes, desplazar mientras se arrastra.
    if (y < headerHeight() + 24.0F * m_scale) {
      scrollBy(-18.0F * m_scale);
    } else if (y > height() - 24.0F * m_scale) {
      scrollBy(18.0F * m_scale);
    }
    if (m_banding) {
      updateRubberBand();
      return;
    }
    const float dx = x - m_pressX;
    const float dy = (y - headerHeight() + m_state->scrollY) - m_pressY;
    if (!m_dragging && m_pressIndex && std::hypot(dx, dy) > Style::dragStartThreshold * m_scale) {
      if (!m_state->selected.contains((*m_entries)[*m_pressIndex].uri)) {
        applyPressSelection(*m_pressIndex, 0);
      }
      // Arrastre de Wayland: el compositor se queda con el puntero (se puede soltar en otras apps).
      if (m_onDragStart) {
        std::vector<std::string> uris;
        for (const std::size_t i : selectedIndices()) {
          uris.push_back((*m_entries)[i].uri);
        }
        if (m_onDragStart(std::move(uris), m_pressSerial)) {
          m_pressed = false;
          m_lastClickIndex.reset();
          return;
        }
      }
      m_dragging = true;
    }
    if (m_dragging) {
      std::optional<std::size_t> target;
      const bool inside = x >= 0.0F && y >= 0.0F && x < width() && y < height();
      if (const auto i = inside ? indexAt(x, y) : std::nullopt;
          i && (*m_entries)[*i].directory && !m_state->selected.contains((*m_entries)[*i].uri)) {
        target = i;
      }
      if (target != m_dropIndex) {
        m_dropIndex = target;
        markLayoutDirty();
      }
      if (m_onDragMotion) {
        m_onDragMotion(sceneX, sceneY);
      }
    }
  }

  void FileView::onRelease(float x, float y, float sceneX, float sceneY) {
    if (!m_pressed) {
      return;
    }
    m_pressed = false;
    if (m_banding) {
      m_banding = false;
      m_band->setVisible(false);
      markLayoutDirty();
      return;
    }
    if (m_dragging) {
      m_dragging = false;
      DropRequest drop{.sceneX = sceneX, .sceneY = sceneY, .modifiers = apps::keyboardModifiers()};
      for (const std::size_t i : selectedIndices()) {
        drop.uris.push_back((*m_entries)[i].uri);
      }
      if (m_dropIndex) {
        const FileEntry& target = (*m_entries)[*m_dropIndex];
        drop.targetUri = target.target.empty() ? target.uri : target.target;
      }
      m_dropIndex.reset();
      markLayoutDirty();
      if (m_onDrop && (!drop.targetUri.empty() || x < 0.0F || y < 0.0F || x >= width() || y >= height())) {
        m_onDrop(drop);
      }
      return;
    }
    if (m_pressIndex && m_pressOnSelected && m_pressModifiers == 0) {
      applyPressSelection(*m_pressIndex, 0);
    }
    if (m_pressIndex && m_pressModifiers == 0 && m_onSingleClick) {
      m_onSingleClick(*m_pressIndex);
    }
  }

  bool FileView::containsScene(float sceneX, float sceneY) const {
    float x = 0.0F;
    float y = 0.0F;
    Node::absolutePosition(this, x, y);
    return sceneX >= x && sceneY >= y && sceneX < x + width() && sceneY < y + height();
  }

  std::string FileView::dropTargetAt(float sceneX, float sceneY, bool own) {
    std::optional<std::size_t> target;
    if (m_entries != nullptr && m_state != nullptr && sceneX >= 0.0F && containsScene(sceneX, sceneY)) {
      float x = 0.0F;
      float y = 0.0F;
      Node::absolutePosition(this, x, y);
      if (const auto i = indexAt(sceneX - x, sceneY - y);
          i && (*m_entries)[*i].directory && !(own && m_state->selected.contains((*m_entries)[*i].uri))) {
        target = i;
      }
    }
    if (target != m_dropIndex) {
      m_dropIndex = target;
      markLayoutDirty();
    }
    if (!target) {
      return {};
    }
    const FileEntry& e = (*m_entries)[*target];
    return e.target.empty() ? e.uri : e.target;
  }

  void FileView::updateRubberBand() {
    const float x0 = std::min(m_pressX, m_pointerX);
    const float x1 = std::max(m_pressX, m_pointerX);
    const float cy = m_pointerY - headerHeight() + m_state->scrollY;
    const float y0 = std::min(m_pressY, cy);
    const float y1 = std::max(m_pressY, cy);
    std::set<std::string> selected = m_bandBase;
    const auto [w, h] = itemSize();
    for (std::size_t i = 0; i < m_entries->size(); ++i) {
      const auto [ix, iy] = itemOrigin(i);
      // En la lista basta tocar la fila a lo alto; en la cuadrícula el rectángulo del icono.
      const bool hit = m_mode == ViewMode::Grid ? ix < x1 && ix + w > x0 && iy < y1 && iy + h > y0
                                                : iy < y1 && iy + h > y0;
      if (hit) {
        if (m_pressModifiers & KeyMod::Ctrl && m_bandBase.contains((*m_entries)[i].uri)) {
          selected.erase((*m_entries)[i].uri);
        } else {
          selected.insert((*m_entries)[i].uri);
        }
      }
    }
    if (selected != m_state->selected) {
      m_state->selected = std::move(selected);
      notifySelection();
    }
    m_band->setVisible(true);
    m_band->setPosition(x0, y0 + headerHeight() - m_state->scrollY);
    m_band->setSize(std::max(1.0F, x1 - x0), std::max(1.0F, y1 - y0));
    markLayoutDirty();
  }

  // ── Dibujo ─────────────────────────────────────────────────────────────────────────────────────────

  void FileView::doLayout(Renderer& renderer) {
    if (width() < 1.0F || height() < 1.0F) {
      return;
    }
    m_input->setSize(width(), height());
    m_content->setSize(width(), height());
    clampScroll();
    const float s = m_scale;
    const float header = headerHeight();
    const float viewport = height() - header;
    const float scrollY = m_state != nullptr ? m_state->scrollY : 0.0F;
    const float thickness = m_vbar->reservedThickness();
    m_vbar->setPosition(width() - thickness, header);
    m_vbar->setSize(thickness, viewport);
    m_vbar->update(viewport, contentHeight(), scrollY);
    m_vbar->setVisible(contentHeight() > viewport + 0.5F);

    const auto [itemW, itemH] = itemSize();
    const Columns cols = listColumns(itemW, s, m_showLocation);
    const ColorSpec muted = colorSpecFromRole(ColorRole::OnSurfaceVariant);

    // Encabezado (solo lista).
    m_header->setVisible(m_mode == ViewMode::List);
    if (m_mode == ViewMode::List) {
      m_header->setSize(width(), header);
      if (m_headerLabels.empty()) {
        for (int i = 0; i < 5; ++i) {
          auto label = std::make_unique<Label>();
          label->setHitTestVisible(false);
          label->setMaxLines(1);
          m_headerLabels.push_back(static_cast<Label*>(m_header->addChild(std::move(label))));
        }
        auto line = std::make_unique<Box>();
        line->setHitTestVisible(false);
        m_headerHits.push_back(static_cast<Box*>(m_header->addChild(std::move(line))));
      }
      m_headerHits[0]->setFill(colorSpecFromRole(ColorRole::OutlineVariant, 0.6F));
      m_headerHits[0]->setPosition(kPad * s, header - 1.0F);
      m_headerHits[0]->setSize(itemW, 1.0F);
      const std::array<std::pair<SortKey, const char*>, 5> keys{
          {{SortKey::Name, "apps.files.column.name"},
           {SortKey::Name, "apps.files.column.location"},
           {SortKey::Size, "apps.files.column.size"},
           {SortKey::Modified, "apps.files.column.modified"},
           {SortKey::Type, "apps.files.column.type"}}
      };
      const std::array<float, 5> widths{cols.name, cols.location, cols.size, cols.modified, cols.type};
      float x = kPad * s;
      for (std::size_t i = 0; i < keys.size(); ++i) {
        Label* label = m_headerLabels[i];
        label->setVisible(widths[i] > 0.0F);
        if (widths[i] <= 0.0F) {
          continue;
        }
        std::string text = i18n::tr(keys[i].second);
        if (i != 1 && keys[i].first == m_sort) {
          text += m_descending ? "  ↓" : "  ↑";
        }
        label->setText(text);
        label->setFontSize(Style::fontSizeCaption * s);
        label->setFontWeight(i != 1 && keys[i].first == m_sort ? FontWeight::SemiBold : FontWeight::Normal);
        label->setColor(muted);
        label->setMaxWidth(widths[i] - Style::spaceSm * s);
        label->layout(renderer);
        const float textX = i == 0 ? x + (kListIcon + Style::spaceMd + Style::spaceSm) * s : x + Style::spaceSm * s;
        label->setPosition(std::round(textX), std::round((header - label->height()) * 0.5F));
        x += widths[i];
      }
    }

    // Filas o celdas visibles.
    std::size_t first = 0;
    std::size_t last = 0;
    const std::size_t count = m_entries != nullptr ? m_entries->size() : 0;
    if (count > 0) {
      if (m_mode == ViewMode::Grid) {
        const auto perRow = static_cast<std::size_t>(columns());
        first = static_cast<std::size_t>(std::max(0.0F, scrollY - kPad * s) / cellHeight()) * perRow;
        last = std::min(count, (static_cast<std::size_t>((scrollY + viewport) / cellHeight()) + 1) * perRow);
      } else {
        first = static_cast<std::size_t>(std::max(0.0F, scrollY - kPad * s) / rowHeight());
        last = std::min(count, static_cast<std::size_t>((scrollY + viewport) / rowHeight()) + 1);
      }
    }
    const bool focused = m_input->focused();
    std::size_t used = 0;
    for (std::size_t index = first; index < last; ++index) {
      if (used == m_slots.size()) {
        auto* slot = m_slots.emplace_back(std::make_unique<Slot>()).get();
        slot->root = m_content->addChild(std::make_unique<Node>());
        slot->root->setHitTestVisible(false);
        slot->background = static_cast<Box*>(slot->root->addChild(std::make_unique<Box>()));
        slot->frame = static_cast<Box*>(slot->root->addChild(std::make_unique<Box>()));
        slot->icon = static_cast<Image*>(slot->root->addChild(std::make_unique<Image>()));
        slot->icon->setFit(ImageFit::Contain);
        slot->fallback = static_cast<Glyph*>(slot->root->addChild(std::make_unique<Glyph>()));
        slot->chevron = static_cast<Glyph*>(slot->root->addChild(std::make_unique<Glyph>()));
        for (Label** label : {&slot->name, &slot->location, &slot->size, &slot->modified, &slot->type}) {
          *label = static_cast<Label*>(slot->root->addChild(std::make_unique<Label>()));
          (*label)->setMaxLines(1);
        }
      }
      Slot& slot = *m_slots[used++];
      const FileEntry& e = (*m_entries)[index];
      const bool selected = m_state->selected.contains(e.uri);
      const bool cursor = focused && m_state->cursor == index;
      const bool drop = m_dropIndex == index;
      const auto [ox, oy] = itemOrigin(index);
      slot.root->setVisible(true);
      slot.root->setPosition(ox, header + oy - scrollY);
      slot.root->setSize(itemW, itemH);
      slot.root->setOpacity(m_dimmed.contains(e.uri) ? 0.45F : (e.hidden ? 0.7F : 1.0F));

      slot.background->setSize(itemW - (m_mode == ViewMode::Grid ? 4.0F * s : 0.0F), itemH - 2.0F * s);
      slot.background->setPosition(m_mode == ViewMode::Grid ? 2.0F * s : 0.0F, 1.0F * s);
      slot.background->setRadius(Style::radiusMd * s);
      slot.background->setFill(
          drop       ? colorSpecFromRole(ColorRole::Primary, 0.32F)
          : selected ? colorSpecFromRole(ColorRole::Primary, m_active ? 0.22F : 0.12F)
          : m_hover == index ? colorSpecFromRole(ColorRole::OnSurface, 0.06F)
                             : clearColorSpec()
      );
      slot.background->setBorder(
          drop || cursor ? colorSpecFromRole(ColorRole::Primary, drop ? 0.9F : 0.55F) : clearColorSpec(),
          drop || cursor ? 1.0F * s : 0.0F
      );

      const float iconPx = (m_mode == ViewMode::Grid ? m_iconSize : kListIcon) * s;
      TextureHandle texture = m_icons ? m_icons(renderer, e, static_cast<int>(std::lround(iconPx * 1.5F))) : TextureHandle{};
      // Miniatura (textura opaca): borde ajustado a lo que de verdad se dibuja dentro del cuadro del icono.
      const auto placeFrame = [&](float iconX, float iconY) {
        const bool thumb = texture.valid() && texture.opaque && texture.width > 0 && texture.height > 0;
        slot.frame->setVisible(thumb);
        if (!thumb) {
          return;
        }
        const float aspect = static_cast<float>(texture.width) / static_cast<float>(texture.height);
        const float w = aspect >= 1.0F ? iconPx : iconPx * aspect;
        const float h = aspect >= 1.0F ? iconPx / aspect : iconPx;
        slot.frame->setPosition(std::round(iconX + (iconPx - w) * 0.5F) - 1.0F, std::round(iconY + (iconPx - h) * 0.5F) - 1.0F);
        slot.frame->setSize(std::round(w) + 2.0F, std::round(h) + 2.0F);
        slot.frame->setFill(clearColorSpec());
        slot.frame->setBorder(colorSpecFromRole(ColorRole::OutlineVariant), 1.0F);
        slot.frame->setRadius(2.0F * s);
      };
      slot.icon->setVisible(texture.valid());
      slot.fallback->setVisible(!texture.valid());
      if (texture.valid()) {
        slot.icon->setExternalTexture(renderer, texture);
      } else {
        slot.fallback->setGlyph(e.directory ? "folder" : "file");
        slot.fallback->setGlyphSize(iconPx * 0.8F);
        slot.fallback->setColor(colorSpecFromRole(e.directory ? ColorRole::Primary : ColorRole::OnSurfaceVariant));
      }

      slot.name->setText(e.display);
      slot.name->setFontSize(Style::fontSizeBody * s);
      slot.name->setColor(colorSpecFromRole(ColorRole::OnSurface));
      slot.name->setFontWeight(FontWeight::Normal);

      if (m_mode == ViewMode::Grid) {
        const float iconX = std::round((itemW - iconPx) * 0.5F);
        const float iconY = 8.0F * s;
        slot.icon->setPosition(iconX, iconY);
        slot.icon->setSize(iconPx, iconPx);
        placeFrame(iconX, iconY);
        slot.fallback->layout(renderer);
        slot.fallback->setPosition(std::round((itemW - slot.fallback->width()) * 0.5F),
                                   iconY + std::round((iconPx - slot.fallback->height()) * 0.5F));
        slot.name->setMaxLines(2);
        slot.name->setTextAlign(TextAlign::Center);
        slot.name->setEllipsize(TextEllipsize::Middle);
        slot.name->setFontSize(Style::fontSizeCaption * s);
        slot.name->setMaxWidth(itemW - 10.0F * s);
        slot.name->layout(renderer);
        slot.name->setPosition(std::round((itemW - slot.name->width()) * 0.5F), iconY + iconPx + 6.0F * s);
        for (Label* label : {slot.location, slot.size, slot.modified, slot.type}) {
          label->setVisible(false);
        }
        slot.chevron->setVisible(false);
        continue;
      }

      const float iconX = Style::spaceSm * s;
      slot.icon->setPosition(iconX, std::round((itemH - iconPx) * 0.5F));
      slot.icon->setSize(iconPx, iconPx);
      placeFrame(iconX, std::round((itemH - iconPx) * 0.5F));
      slot.fallback->layout(renderer);
      slot.fallback->setPosition(iconX, std::round((itemH - slot.fallback->height()) * 0.5F));
      const float nameX = iconX + iconPx + Style::spaceMd * s;
      const float chevronW = m_mode == ViewMode::Compact && e.directory ? 18.0F * s : 0.0F;
      const float nameWidth = m_mode == ViewMode::Compact ? itemW - nameX - chevronW - Style::spaceSm * s
                                                          : cols.name - nameX - Style::spaceSm * s;
      slot.name->setMaxLines(1);
      slot.name->setTextAlign(TextAlign::Start);
      slot.name->setEllipsize(TextEllipsize::Middle);
      slot.name->setMaxWidth(std::max(10.0F, nameWidth));
      slot.name->layout(renderer);
      slot.name->setPosition(nameX, std::round((itemH - slot.name->height()) * 0.5F));

      slot.chevron->setVisible(chevronW > 0.0F);
      if (chevronW > 0.0F) {
        slot.chevron->setGlyph("chevron-right");
        slot.chevron->setGlyphSize(14.0F * s);
        slot.chevron->setColor(muted);
        slot.chevron->layout(renderer);
        slot.chevron->setPosition(itemW - chevronW, std::round((itemH - slot.chevron->height()) * 0.5F));
      }

      const bool details = m_mode == ViewMode::List;
      const std::string sizeText = e.directory ? (e.mountable ? "" : "—") : formatSize(e.size);
      const std::array<std::pair<Label*, std::pair<float, std::string>>, 4> cells{{
          {slot.location, {cols.location, e.location.empty() ? "." : e.location}},
          {slot.size, {cols.size, sizeText}},
          {slot.modified, {cols.modified, formatDate(e.modified)}},
          {slot.type, {cols.type, e.typeName}},
      }};
      float x = cols.name;
      for (const auto& [label, cell] : cells) {
        const auto& [w, text] = cell;
        label->setVisible(details && w > 0.0F);
        if (!details || w <= 0.0F) {
          continue;
        }
        label->setText(text);
        label->setFontSize(Style::fontSizeCaption * s);
        label->setColor(muted);
        label->setEllipsize(label == slot.location ? TextEllipsize::Start : TextEllipsize::End);
        label->setMaxWidth(w - Style::spaceMd * s);
        label->layout(renderer);
        label->setPosition(std::round(x + Style::spaceSm * s), std::round((itemH - label->height()) * 0.5F));
        x += w;
      }
    }
    for (std::size_t i = used; i < m_slots.size(); ++i) {
      m_slots[i]->root->setVisible(false);
    }

    m_empty->setVisible(count == 0 && !m_emptyText.empty());
    if (count == 0 && !m_emptyText.empty()) {
      m_empty->setText(m_emptyText);
      m_empty->setFontSize(Style::fontSizeBody * s);
      m_empty->setColor(muted);
      m_empty->setMaxWidth(width() - 40.0F * s);
      m_empty->setMaxLines(3);
      m_empty->layout(renderer);
      m_empty->setPosition(std::round((width() - m_empty->width()) * 0.5F), std::round(header + (viewport - m_empty->height()) * 0.4F));
    }

    m_band->setFill(colorSpecFromRole(ColorRole::Primary, 0.12F));
    m_band->setBorder(colorSpecFromRole(ColorRole::Primary, 0.7F), 1.0F);
  }

} // namespace okaru::files
