#include "apps/pdf/pdf_view.h"

#include "apps/common/okaru_app.h"
#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "render/scene/input_area.h"
#include "ui/controls/box.h"
#include "ui/controls/image.h"
#include "ui/controls/scrollbar.h"
#include "ui/palette.h"

#include <algorithm>
#include <cmath>
#include <cursor-shape-v1-client-protocol.h>
#include <linux/input-event-codes.h>

namespace okaru::pdf {

  namespace {

    constexpr float kGap = 12.0F;
    constexpr float kMargin = 16.0F;
    constexpr double kPointsToPixels = 96.0 / 72.0;
    constexpr double kMinZoom = 0.1;
    constexpr double kMaxZoom = 8.0;
    constexpr int kMaxTextureSide = 8192;
    constexpr int kKeepAround = 4; // páginas con textura más allá de lo visible
    constexpr double kThumbWidth = 110.0;

  } // namespace

  // ── Datos de la pestaña ──────────────────────────────────────────────────────────────────────────────

  const TextLayout& PdfTab::text(int page) {
    auto it = textCache.find(page);
    if (it == textCache.end()) {
      it = textCache.emplace(page, doc->textLayout(page)).first;
    }
    return it->second;
  }

  const std::vector<Link>& PdfTab::links(int page) {
    auto it = linkCache.find(page);
    if (it == linkCache.end()) {
      it = linkCache.emplace(page, doc->links(page)).first;
    }
    return it->second;
  }

  std::string PdfTab::selectedText() {
    if (selectionPage < 0 || selectionFrom == selectionTo) {
      return {};
    }
    const TextLayout& layout = text(selectionPage);
    const std::size_t from = std::min(selectionFrom, selectionTo);
    const std::size_t to = std::min(std::max(selectionFrom, selectionTo), layout.offsets.size());
    if (from >= to) {
      return {};
    }
    const std::size_t start = layout.offsets[from];
    const std::size_t end = to < layout.offsets.size() ? layout.offsets[to] : layout.text.size();
    return layout.text.substr(start, end - start);
  }

  double fitZoom(Fit fit, PageSize size, float width, float height, bool dual) {
    if (size.width <= 0.0 || size.height <= 0.0) {
      return 1.0;
    }
    const double columns = dual ? 2.0 : 1.0;
    const double byWidth = (width - 2.0 * kMargin - (columns - 1.0) * kGap) / (columns * size.width * kPointsToPixels);
    if (fit == Fit::Width) {
      return std::clamp(byWidth, kMinZoom, kMaxZoom);
    }
    const double byHeight = (height - 2.0 * kMargin) / (size.height * kPointsToPixels);
    return std::clamp(std::min(byWidth, byHeight), kMinZoom, kMaxZoom);
  }

  // ── Vista ────────────────────────────────────────────────────────────────────────────────────────────

  PdfView::PdfView() {
    setClipChildren(true);
    auto content = std::make_unique<Node>();
    m_content = addChild(std::move(content));
    auto input = std::make_unique<InputArea>();
    input->setFocusable(true);
    input->setRetainsFocusOnPointerRelease(true);
    input->setOnPress([this](const InputArea::PointerData& data) {
      if (data.pressed) {
        onPress(data.localX, data.localY);
      } else {
        onRelease(data.localX, data.localY);
      }
    });
    input->setOnMotion([this](const InputArea::PointerData& data) {
      if (m_input != nullptr && m_input->pressed()) {
        onDrag(data.localX, data.localY);
      } else {
        onMotion(data.localX, data.localY);
      }
    });
    input->setOnClick([this](const InputArea::PointerData& data) { onRelease(data.localX, data.localY); });
    input->setOnAxisHandler([this](const InputArea::PointerData& data) -> bool {
      if (m_tab == nullptr) {
        return false;
      }
      if (m_presentation) {
        if (m_onAdvance) {
          m_onAdvance(data.axisValue > 0 ? 1 : -1);
        }
        return true;
      }
      // Ctrl+rueda acerca o aleja bajo el puntero.
      if ((apps::keyboardModifiers() & KeyMod::Ctrl) != 0) {
        setZoom(effectiveZoom() * (data.axisValue < 0 ? 1.1 : 1.0 / 1.1), std::pair{data.localX, data.localY});
        return true;
      }
      if (data.axis == 1) {
        scrollBy(static_cast<float>(data.axisValue), 0.0F);
      } else {
        scrollBy(0.0F, static_cast<float>(data.axisValue) * (data.axisSource == 1 ? 1.0F : 1.6F));
      }
      return true;
    });
    // Teclas de desplazamiento (solo con la vista enfocada: en la barra de búsqueda las flechas son del campo).
    input->setOnKeyDown([this](const InputArea::KeyData& key) {
      if (m_tab == nullptr || !key.pressed || (key.modifiers & KeyMod::Ctrl) != 0) {
        return;
      }
      if (m_presentation) {
        return; // la ventana lleva el teclado de la presentación
      }
      const bool shift = (key.modifiers & KeyMod::Shift) != 0;
      switch (key.sym) {
      case XKB_KEY_Down:
        scrollBy(0.0F, 48.0F);
        break;
      case XKB_KEY_Up:
        scrollBy(0.0F, -48.0F);
        break;
      case XKB_KEY_Right:
        scrollBy(48.0F, 0.0F);
        break;
      case XKB_KEY_Left:
        scrollBy(-48.0F, 0.0F);
        break;
      case XKB_KEY_Page_Down:
        pageStep(1);
        break;
      case XKB_KEY_Page_Up:
        pageStep(-1);
        break;
      case XKB_KEY_space:
        pageStep(shift ? -1 : 1);
        break;
      case XKB_KEY_Home:
        goToPage(0);
        break;
      case XKB_KEY_End:
        goToPage(static_cast<int>(m_tab->sizes.size()) - 1);
        break;
      default:
        return;
      }
    });
    m_input = static_cast<InputArea*>(addChild(std::move(input)));

    // Barras de desplazamiento por encima de las páginas; la horizontal solo aparece con mucho zoom (Scrollbar se
    // oculta sola cuando todo cabe).
    for (const auto orientation : {ScrollOrientation::Vertical, ScrollOrientation::Horizontal}) {
      auto bar = std::make_unique<Scrollbar>();
      bar->setOrientation(orientation);
      bar->setZIndex(5);
      bar->setOnScrollChanged([this, orientation](float offset) {
        if (m_tab == nullptr) {
          return;
        }
        (orientation == ScrollOrientation::Vertical ? m_tab->scrollY : m_tab->scrollX) = offset;
        markLayoutDirty();
      });
      auto* raw = static_cast<Scrollbar*>(addChild(std::move(bar)));
      (orientation == ScrollOrientation::Vertical ? m_vbar : m_hbar) = raw;
    }
  }

  PdfView::~PdfView() = default;

  void PdfView::setSize(float width, float height) {
    Node::setSize(width, height);
    m_input->setSize(width, height);
    markLayoutDirty();
  }

  PageSize PdfView::rotatedSize(int page) const {
    const PageSize size = m_tab->sizes[static_cast<std::size_t>(page)];
    return m_tab->rotation % 180 == 0 ? size : PageSize{size.height, size.width};
  }

  double PdfView::effectiveZoom() const {
    if (m_tab == nullptr || m_tab->sizes.empty()) {
      return 1.0;
    }
    if (m_presentation) {
      return fitZoom(Fit::Page, rotatedSize(m_tab->currentPage), width(), height(), false);
    }
    if (m_tab->fit == Fit::Free) {
      return m_tab->zoom;
    }
    // El ajuste se mide con la página más ancha, como en Papers.
    PageSize widest = rotatedSize(0);
    for (int i = 1; i < static_cast<int>(m_tab->sizes.size()) && i < 64; ++i) {
      const PageSize size = rotatedSize(i);
      if (size.width > widest.width) {
        widest = size;
      }
    }
    return fitZoom(m_tab->fit, widest, width(), height(), m_tab->dual);
  }

  float PdfView::pxPerPoint() const { return static_cast<float>(effectiveZoom() * kPointsToPixels); }

  void PdfView::computeLayout() {
    const float scale = pxPerPoint();
    // Cambió la escala (ventana redimensionada, ajuste): la parte de arriba de la vista se deja en la misma página y
    // a la misma altura de ella. El ancla vive en la pestaña: al redimensionar se reconstruye la vista entera.
    const bool rescaled = m_tab->layoutScale > 0.0F && scale != m_tab->layoutScale && !m_presentation;
    if (rescaled) {
      m_tab->scrollX *= scale / m_tab->layoutScale;
    }
    m_tab->layoutScale = scale;
    m_placed.clear();
    const int count = static_cast<int>(m_tab->sizes.size());
    if (m_presentation) {
      // Una sola página, centrada.
      const PageSize size = rotatedSize(m_tab->currentPage);
      const float w = static_cast<float>(size.width) * scale;
      const float h = static_cast<float>(size.height) * scale;
      m_placed.push_back({m_tab->currentPage, (width() - w) * 0.5F, (height() - h) * 0.5F, w, h});
      m_contentWidth = width();
      m_contentHeight = height();
      return;
    }
    const int columns = m_tab->dual ? 2 : 1;
    float y = kMargin;
    float maxRow = 0.0F;
    for (int first = 0; first < count; first += columns) {
      float rowWidth = 0.0F;
      float rowHeight = 0.0F;
      for (int i = first; i < std::min(count, first + columns); ++i) {
        const PageSize size = rotatedSize(i);
        rowWidth += static_cast<float>(size.width) * scale + (i > first ? kGap : 0.0F);
        rowHeight = std::max(rowHeight, static_cast<float>(size.height) * scale);
      }
      maxRow = std::max(maxRow, rowWidth);
      float x = std::max(kMargin, (width() - rowWidth) * 0.5F);
      for (int i = first; i < std::min(count, first + columns); ++i) {
        const PageSize size = rotatedSize(i);
        const float w = static_cast<float>(size.width) * scale;
        const float h = static_cast<float>(size.height) * scale;
        m_placed.push_back({i, x, y + (rowHeight - h) * 0.5F, w, h});
        x += w + kGap;
      }
      y += rowHeight + kGap;
    }
    m_contentWidth = std::max(width(), maxRow + 2.0F * kMargin);
    m_contentHeight = y - kGap + kMargin;
    if (rescaled) {
      const auto it = std::ranges::find(m_placed, m_tab->anchorPage, &Placed::page);
      if (it != m_placed.end()) {
        m_tab->scrollY = it->y + m_tab->anchorFraction * it->height;
      }
    }
  }

  void PdfView::clampScroll() {
    if (m_tab == nullptr) {
      return;
    }
    m_tab->scrollY = std::clamp(m_tab->scrollY, 0.0F, std::max(0.0F, m_contentHeight - height()));
    m_tab->scrollX = std::clamp(m_tab->scrollX, 0.0F, std::max(0.0F, m_contentWidth - width()));
  }

  void PdfView::updateCurrentPage() {
    if (m_presentation || m_placed.empty()) {
      return;
    }
    // La página que ocupa la línea a un tercio de la altura es la actual.
    const float line = m_tab->scrollY + height() / 3.0F;
    int page = m_placed.front().page;
    for (const auto& placed : m_placed) {
      if (placed.y <= line) {
        page = placed.page;
      }
    }
    if (page != m_tab->currentPage) {
      m_tab->currentPage = page;
      if (m_onPageChanged) {
        m_onPageChanged();
      }
    }
  }

  void PdfView::goToPage(int page, std::optional<Rect> show) {
    if (m_tab == nullptr || m_tab->sizes.empty()) {
      return;
    }
    page = std::clamp(page, 0, static_cast<int>(m_tab->sizes.size()) - 1);
    m_tab->currentPage = page;
    if (m_presentation || m_placed.empty()) {
      m_tab->pendingPage = page;
      markLayoutDirty();
      if (m_onPageChanged) {
        m_onPageChanged();
      }
      return;
    }
    const auto it = std::ranges::find(m_placed, page, &Placed::page);
    float target = it->y - kMargin;
    if (show.has_value()) {
      // Un resultado de búsqueda: a un tercio de la altura.
      const Rect view = toView(*it, *show);
      target = view.y0 - height() / 3.0F;
      if (view.x0 < m_tab->scrollX || view.x1 > m_tab->scrollX + width()) {
        m_tab->scrollX = static_cast<float>(view.x0) - width() / 3.0F;
      }
    }
    m_tab->scrollY = target;
    clampScroll();
    markLayoutDirty();
    if (m_onPageChanged) {
      m_onPageChanged();
    }
  }

  void PdfView::scrollBy(float dx, float dy) {
    if (m_tab == nullptr) {
      return;
    }
    m_tab->scrollX += dx;
    m_tab->scrollY += dy;
    clampScroll();
    markLayoutDirty();
  }

  void PdfView::pageStep(int direction) { scrollBy(0.0F, static_cast<float>(direction) * (height() - 48.0F)); }

  void PdfView::setZoom(double zoom, std::optional<std::pair<float, float>> anchor) {
    if (m_tab == nullptr) {
      return;
    }
    const double old = effectiveZoom();
    zoom = std::clamp(zoom, kMinZoom, kMaxZoom);
    // El punto bajo el puntero (o el centro) se queda donde está.
    const auto [ax, ay] = anchor.value_or(std::pair{width() * 0.5F, height() * 0.5F});
    const float docX = m_tab->scrollX + ax;
    const float docY = m_tab->scrollY + ay;
    const auto ratio = static_cast<float>(zoom / old);
    m_tab->fit = Fit::Free;
    m_tab->zoom = zoom;
    computeLayout();
    m_tab->scrollX = docX * ratio - ax;
    m_tab->scrollY = (docY - kMargin) * ratio + kMargin - ay;
    clampScroll();
    markLayoutDirty();
    if (m_onZoomChanged) {
      m_onZoomChanged();
    }
  }

  void PdfView::setFit(Fit fit) {
    if (m_tab == nullptr) {
      return;
    }
    const int page = m_tab->currentPage;
    m_tab->fit = fit;
    computeLayout();
    goToPage(page);
    if (m_onZoomChanged) {
      m_onZoomChanged();
    }
  }

  Rect PdfView::toView(const Placed& placed, const Rect& rect) const {
    // Puntos de la página sin girar → píxeles de la vista, con el giro aplicado.
    const PageSize size = m_tab->sizes[static_cast<std::size_t>(placed.page)];
    const auto s = static_cast<double>(pxPerPoint());
    double x0 = rect.x0;
    double y0 = rect.y0;
    double x1 = rect.x1;
    double y1 = rect.y1;
    switch (m_tab->rotation) {
    case 90:
      std::tie(x0, y0, x1, y1) = std::tuple{size.height - rect.y1, rect.x0, size.height - rect.y0, rect.x1};
      break;
    case 180:
      std::tie(x0, y0, x1, y1) =
          std::tuple{size.width - rect.x1, size.height - rect.y1, size.width - rect.x0, size.height - rect.y0};
      break;
    case 270:
      std::tie(x0, y0, x1, y1) = std::tuple{rect.y0, size.width - rect.x1, rect.y1, size.width - rect.x0};
      break;
    default:
      break;
    }
    const double left = placed.x - m_tab->scrollX;
    const double top = placed.y - m_tab->scrollY;
    return {left + x0 * s, top + y0 * s, left + x1 * s, top + y1 * s};
  }

  std::optional<std::pair<int, std::pair<double, double>>> PdfView::hit(float x, float y) const {
    const auto s = static_cast<double>(pxPerPoint());
    for (const auto& placed : m_placed) {
      const float left = placed.x - m_tab->scrollX;
      const float top = placed.y - m_tab->scrollY;
      if (x < left || x > left + placed.width || y < top || y > top + placed.height) {
        continue;
      }
      double px = (x - left) / s;
      double py = (y - top) / s;
      const PageSize size = m_tab->sizes[static_cast<std::size_t>(placed.page)];
      // Deshacer el giro: coordenadas de la página tal como está en el archivo.
      switch (m_tab->rotation) {
      case 90:
        std::tie(px, py) = std::pair{py, size.height - px};
        break;
      case 180:
        std::tie(px, py) = std::pair{size.width - px, size.height - py};
        break;
      case 270:
        std::tie(px, py) = std::pair{size.width - py, px};
        break;
      default:
        break;
      }
      return std::pair{placed.page, std::pair{px, py}};
    }
    return std::nullopt;
  }

  std::optional<std::size_t> PdfView::charAt(int page, double px, double py) {
    const TextLayout& layout = m_tab->text(page);
    // El carácter que contiene el punto; si no, el más cercano en la misma línea.
    std::optional<std::size_t> best;
    double bestDistance = 1e18;
    for (std::size_t i = 0; i < layout.chars.size(); ++i) {
      const Rect& r = layout.chars[i];
      if (r.x1 <= r.x0) {
        continue; // saltos de línea sin ancho
      }
      const double dx = px < r.x0 ? r.x0 - px : px > r.x1 ? px - r.x1 : 0.0;
      const double dy = py < r.y0 ? r.y0 - py : py > r.y1 ? py - r.y1 : 0.0;
      const double distance = dy * 4.0 + dx; // lo vertical pesa más: se prefiere la misma línea
      if (distance < bestDistance) {
        bestDistance = distance;
        best = px > (r.x0 + r.x1) * 0.5 ? i + 1 : i;
      }
    }
    return best;
  }

  void PdfView::onPress(float x, float y) {
    if (m_presentation) {
      if (m_onAdvance) {
        m_onAdvance(1);
      }
      return;
    }
    m_pressedOnLink = false;
    const auto at = hit(x, y);
    if (!at) {
      return;
    }
    for (const auto& link : m_tab->links(at->first)) {
      if (link.area.contains(at->second.first, at->second.second)) {
        m_pressedOnLink = true;
        return;
      }
    }
    m_anchorChar = charAt(at->first, at->second.first, at->second.second);
    m_tab->selectionPage = m_anchorChar ? at->first : -1;
    m_tab->selectionFrom = m_tab->selectionTo = m_anchorChar.value_or(0);
    m_selecting = m_anchorChar.has_value();
    markLayoutDirty();
    if (m_onSelectionChanged) {
      m_onSelectionChanged();
    }
  }

  void PdfView::onDrag(float x, float y) {
    if (!m_selecting) {
      return;
    }
    // La selección no sale de la página donde empezó (como en Papers con una página a la vez).
    const auto it = std::ranges::find(m_placed, m_tab->selectionPage, &Placed::page);
    if (it == m_placed.end()) {
      return;
    }
    const float left = it->x - m_tab->scrollX;
    const float top = it->y - m_tab->scrollY;
    const auto at = hit(std::clamp(x, left, left + it->width - 1.0F), std::clamp(y, top, top + it->height - 1.0F));
    if (at && at->first == m_tab->selectionPage) {
      if (const auto index = charAt(at->first, at->second.first, at->second.second)) {
        m_tab->selectionTo = *index;
        markLayoutDirty();
      }
    }
    // Arrastrar hasta el borde desplaza.
    if (y < 0.0F || y > height()) {
      scrollBy(0.0F, y < 0.0F ? y * 0.3F : (y - height()) * 0.3F);
    }
  }

  void PdfView::onRelease(float x, float y) {
    if (m_selecting) {
      m_selecting = false;
      if (m_onSelectionChanged) {
        m_onSelectionChanged();
      }
      return;
    }
    if (!m_pressedOnLink) {
      return;
    }
    m_pressedOnLink = false;
    const auto at = hit(x, y);
    if (!at) {
      return;
    }
    for (const auto& link : m_tab->links(at->first)) {
      if (link.area.contains(at->second.first, at->second.second)) {
        if (link.page >= 0) {
          goToPage(link.page);
        } else if (!link.uri.empty() && m_onOpenUri) {
          m_onOpenUri(link.uri);
        }
        return;
      }
    }
  }

  void PdfView::onMotion(float x, float y) {
    if (m_presentation) {
      return;
    }
    // Mano sobre los enlaces, cursor de texto sobre la página.
    std::uint32_t shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
    if (const auto at = hit(x, y)) {
      shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT;
      for (const auto& link : m_tab->links(at->first)) {
        if (link.area.contains(at->second.first, at->second.second)) {
          shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER;
          break;
        }
      }
    }
    m_input->setCursorShape(shape);
  }

  void PdfView::uploadArrived(Renderer& renderer) {
    auto& textures = renderer.textureManager();
    for (auto& [job, page] : m_tab->arrived) {
      auto& slots = job.thumbnail ? m_tab->thumbs : m_tab->pages;
      if (job.page < 0 || static_cast<std::size_t>(job.page) >= slots.size() || page.rgba.empty()) {
        continue;
      }
      PageTexture& slot = slots[static_cast<std::size_t>(job.page)];
      if (slot.handle.valid()) {
        textures.unload(slot.handle);
      }
      slot.handle = textures.loadFromRgba(page.rgba.data(), page.width, page.height, true);
      slot.job = job;
    }
    m_tab->arrived.clear();
  }

  void PdfView::requestRenders() {
    if (m_tab->worker == nullptr || m_placed.empty()) {
      return;
    }
    const auto scale = static_cast<double>(pxPerPoint() * m_bufferScale);
    const float top = m_tab->scrollY;
    const float bottom = m_tab->scrollY + height();
    std::vector<RenderJob> jobs;
    std::vector<int> wanted;
    // Primero lo visible, después una página más arriba y abajo (para desplazarse sin esperar).
    for (const auto& placed : m_placed) {
      if (placed.y + placed.height >= top && placed.y <= bottom) {
        wanted.push_back(placed.page);
      }
    }
    if (!wanted.empty()) {
      m_firstVisible = wanted.front();
      m_lastVisible = wanted.back();
      const int first = wanted.front();
      const int last = wanted.back();
      for (const int extra : {last + 1, first - 1, last + 2}) {
        if (extra >= 0 && extra < static_cast<int>(m_tab->sizes.size()) && !m_presentation) {
          wanted.push_back(extra);
        }
      }
    }
    for (const int page : wanted) {
      const PageSize size = m_tab->sizes[static_cast<std::size_t>(page)];
      // Una página enorme muy ampliada se dibuja hasta el lado máximo de textura (se verá algo más suave).
      const double maxScale = kMaxTextureSide / std::max(size.width, size.height);
      const RenderJob job{page, std::min(scale, maxScale), m_tab->rotation, m_tab->night, false};
      const PageTexture& slot = m_tab->pages[static_cast<std::size_t>(page)];
      const bool stale = !slot.handle.valid()
          || slot.job.rotation != job.rotation
          || slot.job.invert != job.invert
          || std::abs(slot.job.scale - job.scale) > job.scale * 0.04;
      if (stale) {
        jobs.push_back(job);
      }
    }
    // Miniaturas de a poco, después de las páginas.
    int queued = 0;
    for (std::size_t i = 0; i < m_tab->thumbs.size() && queued < 12; ++i) {
      const PageTexture& slot = m_tab->thumbs[i];
      if (!slot.handle.valid() || slot.job.rotation != m_tab->rotation || slot.job.invert != m_tab->night) {
        const PageSize size = m_tab->sizes[i];
        jobs.push_back(
            {static_cast<int>(i), kThumbWidth * m_bufferScale / size.width, m_tab->rotation, m_tab->night, true}
        );
        ++queued;
      }
    }
    m_tab->worker->request(std::move(jobs));
  }

  void PdfView::doLayout(Renderer& renderer) {
    // Sin tamaño todavía (la fila aún no lo repartió): nada que ubicar.
    if (m_tab == nullptr || m_tab->doc == nullptr || width() < 1.0F || height() < 1.0F) {
      return;
    }
    uploadArrived(renderer);
    computeLayout();
    if (m_tab->pendingPage.has_value() && !m_presentation) {
      const int page = *m_tab->pendingPage;
      m_tab->pendingPage.reset();
      goToPage(page);
    }
    clampScroll();
    for (const auto& placed : m_placed) {
      if (placed.y + placed.height >= m_tab->scrollY) {
        m_tab->anchorPage = placed.page;
        m_tab->anchorFraction = (m_tab->scrollY - placed.y) / placed.height;
        break;
      }
    }
    updateCurrentPage();
    // El zoom de "ajustar" depende del tamaño de la vista: se avisa cuando ya se conoce.
    if (const double zoom = effectiveZoom(); zoom != m_reportedZoom) {
      m_reportedZoom = zoom;
      if (m_onZoomChanged) {
        m_onZoomChanged();
      }
    }
    refresh(renderer);
    requestRenders();
    // Texturas lejos de lo visible se liberan (un PDF de cientos de páginas no llena la GPU).
    auto& textures = renderer.textureManager();
    for (std::size_t i = 0; i < m_tab->pages.size(); ++i) {
      const auto page = static_cast<int>(i);
      if ((page < m_firstVisible - kKeepAround || page > m_lastVisible + kKeepAround)
          && m_tab->pages[i].handle.valid()) {
        textures.unload(m_tab->pages[i].handle);
        m_tab->pages[i] = {};
      }
    }
    Node::doLayout(renderer);
  }

  void PdfView::refresh(Renderer& renderer) {
    m_content->setSize(width(), height());
    m_input->setSize(width(), height());
    std::size_t used = 0;
    std::size_t marks = 0;
    const auto mark = [&](const Rect& r, const ColorSpec& color) {
      if (marks == m_marks.size()) {
        auto box = std::make_unique<Box>();
        box->setHitTestVisible(false);
        m_marks.push_back(static_cast<Box*>(m_content->addChild(std::move(box))));
      }
      Box* box = m_marks[marks++];
      box->setVisible(true);
      box->setFill(color);
      box->setZIndex(2);
      box->setPosition(static_cast<float>(r.x0), static_cast<float>(r.y0));
      box->setSize(static_cast<float>(r.x1 - r.x0), static_cast<float>(r.y1 - r.y0));
    };
    for (const auto& placed : m_placed) {
      const float left = placed.x - m_tab->scrollX;
      const float top = placed.y - m_tab->scrollY;
      if (top + placed.height < 0.0F || top > height()) {
        continue;
      }
      if (used == m_images.size()) {
        auto paper = std::make_unique<Box>();
        paper->setHitTestVisible(false);
        m_paper.push_back(static_cast<Box*>(m_content->addChild(std::move(paper))));
        auto image = std::make_unique<Image>();
        image->setFit(ImageFit::Stretch);
        image->setHitTestVisible(false);
        m_images.push_back(static_cast<Image*>(m_content->addChild(std::move(image))));
      }
      Box* paper = m_paper[used];
      Image* image = m_images[used];
      ++used;
      // Papel (se ve mientras llega la imagen) con un borde suave.
      paper->setVisible(true);
      paper->setFill(m_tab->night ? colorForRole(ColorRole::Shadow) : rgba(1.0F, 1.0F, 1.0F, 1.0F));
      paper->setBorder(colorSpecFromRole(ColorRole::OutlineVariant, 0.6F), 1.0F);
      paper->setPosition(std::round(left) - 1.0F, std::round(top) - 1.0F);
      paper->setSize(std::round(placed.width) + 2.0F, std::round(placed.height) + 2.0F);
      const PageTexture& slot = m_tab->pages[static_cast<std::size_t>(placed.page)];
      image->setVisible(slot.handle.valid());
      if (slot.handle.valid()) {
        image->setExternalTexture(renderer, slot.handle);
      }
      image->setPosition(std::round(left), std::round(top));
      image->setSize(std::round(placed.width), std::round(placed.height));
      image->setZIndex(1);
      paper->setZIndex(0);

      // Resultados de búsqueda (el actual más fuerte) y selección de texto.
      for (std::size_t h = 0; h < m_tab->hits.size(); ++h) {
        if (m_tab->hits[h].page == placed.page) {
          const bool current = static_cast<int>(h) == m_tab->hitIndex;
          mark(
              toView(placed, m_tab->hits[h].rect),
              colorSpecFromRole(current ? ColorRole::Primary : ColorRole::Tertiary, current ? 0.45F : 0.28F)
          );
        }
      }
      if (m_tab->selectionPage == placed.page && m_tab->selectionFrom != m_tab->selectionTo) {
        const TextLayout& layout = m_tab->text(placed.page);
        const std::size_t from = std::min(m_tab->selectionFrom, m_tab->selectionTo);
        const std::size_t to = std::min(std::max(m_tab->selectionFrom, m_tab->selectionTo), layout.chars.size());
        for (std::size_t c = from; c < to; ++c) {
          if (layout.chars[c].x1 > layout.chars[c].x0) {
            mark(toView(placed, layout.chars[c]), colorSpecFromRole(ColorRole::Primary, 0.30F));
          }
        }
      }
    }
    // Barras: la vertical a la derecha, la horizontal abajo (dejando la esquina libre si están las dos).
    const bool wide = !m_presentation && m_contentWidth > width() + 0.5F;
    const bool tall = !m_presentation && m_contentHeight > height() + 0.5F;
    const float thickness = m_vbar->reservedThickness();
    m_vbar->setVisible(tall);
    m_hbar->setVisible(wide);
    m_vbar->setPosition(width() - thickness, 0.0F);
    m_vbar->setSize(thickness, height() - (wide ? thickness : 0.0F));
    m_vbar->update(height() - (wide ? thickness : 0.0F), m_contentHeight - (wide ? thickness : 0.0F), m_tab->scrollY);
    m_hbar->setPosition(0.0F, height() - thickness);
    m_hbar->setSize(width() - (tall ? thickness : 0.0F), thickness);
    m_hbar->update(width() - (tall ? thickness : 0.0F), m_contentWidth - (tall ? thickness : 0.0F), m_tab->scrollX);
    for (std::size_t i = used; i < m_images.size(); ++i) {
      m_images[i]->setVisible(false);
      m_paper[i]->setVisible(false);
    }
    for (std::size_t i = marks; i < m_marks.size(); ++i) {
      m_marks[i]->setVisible(false);
    }
    markPaintDirty();
  }

} // namespace okaru::pdf
