#include "apps/files/drag_icon.h"

#include <algorithm>
#include <cairo.h>
#include <cmath>
#include <cstring>
#include <gdk-pixbuf/gdk-pixbuf.h>

namespace okaru::files {

  namespace {

    constexpr float kIconSize = 64.0F;
    constexpr float kStackStep = 7.0F; // desplazamiento de cada copia de la pila
    constexpr float kBadgeRadius = 11.0F;

    // GdkPixbuf (RGBA sin premultiplicar) → superficie de cairo (ARGB premultiplicado).
    cairo_surface_t* surfaceFromPixbuf(GdkPixbuf* pixbuf) {
      GdkPixbuf* rgba = gdk_pixbuf_add_alpha(pixbuf, FALSE, 0, 0, 0);
      const int w = gdk_pixbuf_get_width(rgba);
      const int h = gdk_pixbuf_get_height(rgba);
      const int srcStride = gdk_pixbuf_get_rowstride(rgba);
      const guchar* src = gdk_pixbuf_get_pixels(rgba);
      cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
      auto* dst = cairo_image_surface_get_data(surface);
      const int dstStride = cairo_image_surface_get_stride(surface);
      for (int y = 0; y < h; ++y) {
        const guchar* in = src + static_cast<std::ptrdiff_t>(y) * srcStride;
        auto* out = reinterpret_cast<std::uint32_t*>(dst + static_cast<std::ptrdiff_t>(y) * dstStride);
        for (int x = 0; x < w; ++x, in += 4) {
          const std::uint32_t a = in[3];
          const auto pre = [a](guchar c) { return (static_cast<std::uint32_t>(c) * a + 127) / 255; };
          out[x] = (a << 24U) | (pre(in[0]) << 16U) | (pre(in[1]) << 8U) | pre(in[2]);
        }
      }
      cairo_surface_mark_dirty(surface);
      g_object_unref(rgba);
      return surface;
    }

  } // namespace

  apps::DragIcon makeDragIcon(
      const std::vector<std::string>& images, std::size_t count, const Color& badge, const Color& onBadge, float scale
  ) {
    const int iconPx = static_cast<int>(std::lround(kIconSize * scale));
    const std::size_t layers = std::min<std::size_t>(images.size(), 3);
    if (layers == 0) {
      return {};
    }
    const float step = kStackStep * scale;
    const float radius = kBadgeRadius * scale;
    const int width = iconPx + static_cast<int>(std::ceil(step * static_cast<float>(layers - 1) + radius));
    const int height = width;

    cairo_surface_t* canvas = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cairo_t* cr = cairo_create(canvas);
    const float top = radius; // sitio para la insignia arriba

    // De atrás hacia adelante: la primera imagen queda encima.
    for (std::size_t i = layers; i-- > 0;) {
      GdkPixbuf* pixbuf = gdk_pixbuf_new_from_file_at_size(images[i].c_str(), iconPx, iconPx, nullptr);
      if (pixbuf == nullptr) {
        continue;
      }
      cairo_surface_t* image = surfaceFromPixbuf(pixbuf);
      const int w = gdk_pixbuf_get_width(pixbuf);
      const int h = gdk_pixbuf_get_height(pixbuf);
      g_object_unref(pixbuf);
      const double x = step * static_cast<float>(i) + (iconPx - w) / 2.0;
      const double y = top - radius + step * static_cast<float>(i) + (iconPx - h) / 2.0;
      cairo_set_source_surface(cr, image, x, y);
      cairo_paint_with_alpha(cr, i == 0 ? 0.92 : 0.6);
      cairo_surface_destroy(image);
    }

    if (count > 1) {
      const double cx = width - radius;
      const double cy = radius;
      cairo_arc(cr, cx, cy, radius, 0.0, 2.0 * M_PI);
      cairo_set_source_rgba(cr, badge.r, badge.g, badge.b, 1.0);
      cairo_fill(cr);
      const std::string text = count > 99 ? "99+" : std::to_string(count);
      cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
      cairo_set_font_size(cr, (text.size() > 2 ? 9.0 : 12.0) * scale);
      cairo_text_extents_t extents;
      cairo_text_extents(cr, text.c_str(), &extents);
      cairo_move_to(cr, cx - extents.width / 2.0 - extents.x_bearing, cy - extents.height / 2.0 - extents.y_bearing);
      cairo_set_source_rgba(cr, onBadge.r, onBadge.g, onBadge.b, 1.0);
      cairo_show_text(cr, text.c_str());
    }
    cairo_destroy(cr);
    cairo_surface_flush(canvas);

    apps::DragIcon icon{
        .width = width,
        .height = height,
        .hotspotX = iconPx / 3,
        .hotspotY = static_cast<int>(top) + iconPx / 3,
        .pixels = std::vector<std::uint32_t>(static_cast<std::size_t>(width) * height)
    };
    const auto* data = cairo_image_surface_get_data(canvas);
    const int stride = cairo_image_surface_get_stride(canvas);
    for (int y = 0; y < height; ++y) {
      std::memcpy(
          icon.pixels.data() + static_cast<std::size_t>(y) * width, data + static_cast<std::ptrdiff_t>(y) * stride,
          static_cast<std::size_t>(width) * 4
      );
    }
    cairo_surface_destroy(canvas);
    return icon;
  }

} // namespace okaru::files
