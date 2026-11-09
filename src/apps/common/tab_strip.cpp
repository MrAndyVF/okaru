#include "apps/common/tab_strip.h"

#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>

namespace okaru::apps {

  std::unique_ptr<Node> tabStrip(Renderer* renderer, float width, float scale, TabStripProps props) {
    const float padding = Style::spaceMd * scale;
    const float gap = Style::spaceXs * scale;
    const float height = Style::controlHeightSm * scale;
    const auto count = static_cast<float>(props.tabs.size());
    // Todas del mismo ancho, como en GNOME, hasta 240 px.
    const float tabWidth = std::min(240.0F * scale, (width - 2.0F * padding - gap * (count - 1.0F)) / count);
    const float side = 22.0F * scale;
    const float radius = Style::radiusMd * scale;

    auto tabs = ui::row({
        .align = FlexAlign::Center,
        .gap = gap,
        .paddingV = 3.0F * scale,
        .paddingH = padding,
        .fill = colorSpecFromRole(ColorRole::SurfaceContainerLow),
    });
    for (std::size_t i = 0; i < props.tabs.size(); ++i) {
      const bool active = i == props.active;
      auto tab = ui::node({.width = tabWidth, .height = height});

      // Fondo: la activa en el color primario; las demás se marcan apenas al pasar el puntero.
      Box* background = nullptr;
      const ColorSpec restFill = active ? colorSpecFromRole(ColorRole::Primary) : clearColorSpec();
      const ColorSpec hoverFill = active ? restFill : colorSpecFromRole(ColorRole::OnSurface, 0.06F);
      tab->addChild(
          ui::box({.out = &background, .fill = restFill, .radius = radius, .width = tabWidth, .height = height})
      );
      auto area = ui::inputArea({
          .tooltip = props.tabs[i].tooltip,
          .width = tabWidth,
          .height = height,
          .onClick = [onActivate = props.onActivate, i](const InputArea::PointerData&) { onActivate(i); },
      });
      area->setOnEnter([background, hoverFill](const InputArea::PointerData&) { background->setFill(hoverFill); });
      area->setOnLeave([background, restFill] { background->setFill(restFill); });
      tab->addChild(std::move(area));

      // Nombre centrado; se acorta si no cabe sin tapar la X.
      const ColorRole text = active ? ColorRole::OnPrimary : ColorRole::OnSurface;
      auto label = ui::label({
          .text = props.tabs[i].title,
          .fontSize = Style::fontSizeCaption * scale,
          .fontWeight = active ? FontWeight::SemiBold : FontWeight::Normal,
          .color = colorSpecFromRole(text),
          .maxWidth = tabWidth - 2.0F * (side + 8.0F * scale),
          .maxLines = 1,
          .ellipsize = TextEllipsize::Middle,
      });
      label->setHitTestVisible(false);
      if (renderer != nullptr) {
        label->layout(*renderer);
      }
      label->setPosition(std::round((tabWidth - label->width()) * 0.5F), std::round((height - label->height()) * 0.5F));
      tab->addChild(std::move(label));

      // X a la derecha: su recuadro se oscurece al pasar el puntero (en la activa, un tono más oscuro del primario).
      const float closeX = tabWidth - side - 6.0F * scale;
      const float closeY = (height - side) * 0.5F;
      Box* closeBox = nullptr;
      const ColorSpec closeHover =
          active ? colorSpecFromRole(ColorRole::Shadow, 0.22F) : colorSpecFromRole(ColorRole::OnSurface, 0.12F);
      auto closeBackground =
          ui::box({.out = &closeBox, .fill = clearColorSpec(), .radius = radius, .width = side, .height = side});
      closeBackground->setPosition(closeX, closeY);
      tab->addChild(std::move(closeBackground));
      const float glyphSize = 14.0F * scale;
      auto icon = ui::glyph({
          .glyph = "x",
          .glyphSize = glyphSize,
          .color = colorSpecFromRole(text, 0.85F),
          .width = glyphSize,
          .height = glyphSize,
      });
      icon->setHitTestVisible(false);
      icon->setPosition(std::round(closeX + (side - glyphSize) * 0.5F), std::round(closeY + (side - glyphSize) * 0.5F));
      tab->addChild(std::move(icon));
      auto close = ui::inputArea({
          .tooltip = props.closeTooltip,
          .width = side,
          .height = side,
          .onClick = [onClose = props.onClose, i](const InputArea::PointerData&) { onClose(i); },
      });
      close->setPosition(closeX, closeY);
      close->setZIndex(1);
      close->setOnEnter([closeBox, closeHover](const InputArea::PointerData&) { closeBox->setFill(closeHover); });
      close->setOnLeave([closeBox] { closeBox->setFill(clearColorSpec()); });
      tab->addChild(std::move(close));
      tabs->addChild(std::move(tab));
    }
    return tabs;
  }

} // namespace okaru::apps
