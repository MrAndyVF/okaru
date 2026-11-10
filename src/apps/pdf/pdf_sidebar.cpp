#include "apps/pdf/pdf_sidebar.h"

#include "render/core/renderer.h"
#include "ui/controls/box.h"
#include "ui/controls/image.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>

namespace okaru::pdf {

  namespace {

    constexpr float kThumbWidth = 110.0F;
    constexpr int kMaxIndent = 5;

    // Fila del índice: fondo (al pasar el puntero) y el título sangrado según su nivel.
    class OutlineRow final : public Node {
    public:
      OutlineRow() {
        m_background = static_cast<Box*>(addChild(std::make_unique<Box>()));
        m_label = static_cast<Label*>(addChild(std::make_unique<Label>()));
        m_label->setMaxLines(1);
        m_label->setEllipsize(TextEllipsize::End);
      }
      Box* m_background = nullptr;
      Label* m_label = nullptr;
    };

    // Celda de miniatura: marco (resaltado si es la página actual), papel, imagen y número.
    class ThumbCell final : public Node {
    public:
      ThumbCell() {
        m_frame = static_cast<Box*>(addChild(std::make_unique<Box>()));
        m_paper = static_cast<Box*>(addChild(std::make_unique<Box>()));
        m_image = static_cast<Image*>(addChild(std::make_unique<Image>()));
        m_image->setFit(ImageFit::Stretch);
        m_number = static_cast<Label*>(addChild(std::make_unique<Label>()));
      }
      Box* m_frame = nullptr;
      Box* m_paper = nullptr;
      Image* m_image = nullptr;
      Label* m_number = nullptr;
    };

  } // namespace

  // ── Índice ─────────────────────────────────────────────────────────────────────────────────────────

  float OutlineAdapter::measureItem(Renderer& /*renderer*/, std::size_t /*index*/, float /*width*/) {
    return Style::controlHeightSm * m_scale;
  }

  std::unique_ptr<Node> OutlineAdapter::createItem() { return std::make_unique<OutlineRow>(); }

  void OutlineAdapter::bindItem(Renderer& renderer, Node& item, std::size_t index, float width, bool hovered) {
    auto& row = static_cast<OutlineRow&>(item);
    const OutlineItem& entry = m_tab.outline[index];
    const float height = Style::controlHeightSm * m_scale;
    row.setSize(width, height);
    row.m_background->setSize(width, height);
    row.m_background->setRadius(Style::radiusMd * m_scale);
    row.m_background->setFill(
        hovered && entry.page >= 0 ? colorSpecFromRole(ColorRole::OnSurface, 0.08F) : clearColorSpec()
    );
    const float indent = (Style::spaceSm + 12.0F * static_cast<float>(std::min(entry.depth, kMaxIndent))) * m_scale;
    row.m_label->setText(entry.title);
    row.m_label->setFontSize(Style::fontSizeCaption * m_scale);
    // Los capítulos (primer nivel) en negrita, como en Papers.
    row.m_label->setFontWeight(entry.depth == 0 ? FontWeight::SemiBold : FontWeight::Normal);
    row.m_label->setColor(colorSpecFromRole(entry.page >= 0 ? ColorRole::OnSurface : ColorRole::OnSurfaceVariant));
    row.m_label->setMaxWidth(std::max(0.0F, width - indent - Style::spaceSm * m_scale));
    row.m_label->layout(renderer);
    row.m_label->setPosition(indent, std::round((height - row.m_label->height()) * 0.5F));
  }

  // ── Miniaturas ─────────────────────────────────────────────────────────────────────────────────────

  float ThumbnailAdapter::thumbHeight(std::size_t index) const {
    PageSize size = m_tab.sizes[index];
    if (m_tab.rotation % 180 != 0) {
      std::swap(size.width, size.height);
    }
    return std::round(kThumbWidth * m_scale * static_cast<float>(size.height / std::max(1.0, size.width)));
  }

  std::uint64_t ThumbnailAdapter::itemRevision(std::size_t index) const {
    const PageTexture& slot = m_tab.thumbs[index];
    return (slot.handle.valid() ? static_cast<std::uint64_t>(slot.handle.id.value()) << 3 : 0U)
        | (static_cast<int>(index) == m_tab.currentPage ? 1U : 0U)
        | (m_tab.night ? 2U : 0U)
        | (static_cast<std::uint64_t>(m_tab.rotation) << 40);
  }

  float ThumbnailAdapter::measureItem(Renderer& /*renderer*/, std::size_t index, float /*width*/) {
    return thumbHeight(index) + 30.0F * m_scale;
  }

  std::unique_ptr<Node> ThumbnailAdapter::createItem() { return std::make_unique<ThumbCell>(); }

  void ThumbnailAdapter::bindItem(Renderer& renderer, Node& item, std::size_t index, float width, bool hovered) {
    auto& cell = static_cast<ThumbCell&>(item);
    const float imageWidth = kThumbWidth * m_scale;
    const float imageHeight = thumbHeight(index);
    const float height = imageHeight + 30.0F * m_scale;
    const bool current = static_cast<int>(index) == m_tab.currentPage;
    cell.setSize(width, height);

    cell.m_frame->setSize(width, height);
    cell.m_frame->setRadius(Style::radiusMd * m_scale);
    cell.m_frame->setFill(
        current       ? colorSpecFromRole(ColorRole::Primary, 0.18F)
            : hovered ? colorSpecFromRole(ColorRole::OnSurface, 0.06F)
                      : clearColorSpec()
    );

    const float left = std::round((width - imageWidth) * 0.5F);
    const float top = 6.0F * m_scale;
    cell.m_paper->setFill(
        m_tab.night ? colorSpecFromRole(ColorRole::Shadow) : fixedColorSpec(rgba(1.0F, 1.0F, 1.0F, 1.0F))
    );
    cell.m_paper->setBorder(colorSpecFromRole(ColorRole::OutlineVariant), 1.0F);
    cell.m_paper->setPosition(left, top);
    cell.m_paper->setSize(imageWidth, imageHeight);

    const PageTexture& slot = m_tab.thumbs[index];
    cell.m_image->setVisible(slot.handle.valid());
    if (slot.handle.valid()) {
      cell.m_image->setExternalTexture(renderer, slot.handle);
    }
    cell.m_image->setPosition(left, top);
    cell.m_image->setSize(imageWidth, imageHeight);

    const std::string label = m_tab.doc->pageLabel(static_cast<int>(index));
    cell.m_number->setText(label.empty() ? std::to_string(index + 1) : label);
    cell.m_number->setFontSize(Style::fontSizeCaption * m_scale);
    cell.m_number->setColor(colorSpecFromRole(current ? ColorRole::Primary : ColorRole::OnSurfaceVariant));
    cell.m_number->layout(renderer);
    cell.m_number->setPosition(std::round((width - cell.m_number->width()) * 0.5F), imageHeight + 9.0F * m_scale);
  }

} // namespace okaru::pdf
