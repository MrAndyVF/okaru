#include "apps/files/preview.h"

#include "core/process/process.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "ui/controls/image.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>
#include <linux/input-event-codes.h>
#include <cursor-shape-v1-client-protocol.h>

namespace okaru::files {

  namespace {

    namespace fs = std::filesystem;

    constexpr float kMaxScale = 16.0F;
    constexpr std::uint32_t kDoubleClickMs = 400;

    fs::path previewCache() { return fs::path(g_get_user_cache_dir()) / "okaru" / "files-preview"; }

  } // namespace

  PreviewKind previewKindFor(const std::string& type) {
    if (type == "application/pdf") {
      return PreviewKind::Pdf;
    }
    // Word, Excel, PowerPoint (viejos y nuevos), OpenDocument y RTF.
    if (type.starts_with("application/vnd.openxmlformats-officedocument.")
        || type.starts_with("application/vnd.oasis.opendocument.") || type.starts_with("application/vnd.ms-excel")
        || type.starts_with("application/vnd.ms-powerpoint") || type.starts_with("application/vnd.ms-word")
        || type == "application/msword" || type == "application/rtf" || type == "text/rtf") {
      return PreviewKind::Office;
    }
    if (type.starts_with("image/")) {
      // Lo que gdk-pixbuf sepa abrir (incluye SVG, WebP, HEIF y AVIF si están sus cargadores).
      static const std::vector<std::string> types = [] {
        std::vector<std::string> out;
        GSList* formats = gdk_pixbuf_get_formats();
        for (GSList* l = formats; l != nullptr; l = l->next) {
          gchar** mimes = gdk_pixbuf_format_get_mime_types(static_cast<GdkPixbufFormat*>(l->data));
          for (gchar** m = mimes; m != nullptr && *m != nullptr; ++m) {
            out.emplace_back(*m);
          }
          g_strfreev(mimes);
        }
        g_slist_free(formats);
        return out;
      }();
      if (std::ranges::find(types, type) != types.end()) {
        return PreviewKind::Image;
      }
    }
    return PreviewKind::None;
  }

  DecodedImage decodeImage(const std::string& path, int maxSize) {
    DecodedImage out;
    (void)gdk_pixbuf_get_file_info(path.c_str(), &out.originalWidth, &out.originalHeight);
    // Sin agrandar: una foto chica se decodifica a su tamaño real.
    const int width = out.originalWidth > 0 ? std::min(maxSize, out.originalWidth) : maxSize;
    const int height = out.originalHeight > 0 ? std::min(maxSize, out.originalHeight) : maxSize;
    GdkPixbuf* loaded = gdk_pixbuf_new_from_file_at_size(path.c_str(), width, height, nullptr);
    if (loaded == nullptr) {
      return out;
    }
    GdkPixbuf* oriented = gdk_pixbuf_apply_embedded_orientation(loaded);
    g_object_unref(loaded);
    GdkPixbuf* rgba = gdk_pixbuf_add_alpha(oriented, FALSE, 0, 0, 0);
    g_object_unref(oriented);
    out.width = gdk_pixbuf_get_width(rgba);
    out.height = gdk_pixbuf_get_height(rgba);
    const int stride = gdk_pixbuf_get_rowstride(rgba);
    const guchar* src = gdk_pixbuf_get_pixels(rgba);
    out.rgba.resize(static_cast<std::size_t>(out.width) * static_cast<std::size_t>(out.height) * 4);
    for (int y = 0; y < out.height; ++y) {
      std::copy_n(src + static_cast<std::size_t>(y) * stride, static_cast<std::size_t>(out.width) * 4,
                  out.rgba.data() + static_cast<std::size_t>(y) * out.width * 4);
    }
    g_object_unref(rgba);
    return out;
  }

  std::string officePreviewPath(const std::string& uri, std::int64_t modified) {
    gchar* sum = g_compute_checksum_for_string(G_CHECKSUM_MD5, uri.c_str(), -1);
    const std::string name = std::string(sum) + "-" + std::to_string(modified) + ".pdf";
    g_free(sum);
    return (previewCache() / name).string();
  }

  bool convertOfficeToPdf(const std::string& path, const std::string& output, std::string& error) {
    std::error_code ec;
    fs::create_directories(previewCache(), ec);
    gchar* tempDir = g_dir_make_tmp("okaru-office-XXXXXX", nullptr);
    if (tempDir == nullptr) {
      error = "tmp";
      return false;
    }
    // Perfil propio: con LibreOffice abierto, una conversión con el perfil normal se entrega a esa instancia y no
    // produce nada.
    const std::string profile = "-env:UserInstallation=file://" + (previewCache().parent_path() / "libreoffice").string();
    const auto result = process::runSyncWithTimeout(
        {"soffice", profile, "--headless", "--norestore", "--nolockcheck", "--convert-to", "pdf", "--outdir", tempDir,
         path},
        std::chrono::seconds(120)
    );
    bool ok = false;
    for (const auto& item : fs::directory_iterator(tempDir, ec)) {
      if (item.path().extension() == ".pdf") {
        fs::rename(item.path(), output, ec);
        if (ec) { // otra partición: copiar
          ec.clear();
          fs::copy_file(item.path(), output, fs::copy_options::overwrite_existing, ec);
        }
        ok = !ec;
        break;
      }
    }
    if (!ok) {
      error = result.err.empty() ? result.out : result.err;
    }
    fs::remove_all(tempDir, ec);
    g_free(tempDir);
    return ok;
  }

  // ── ImagePreview ───────────────────────────────────────────────────────────────────────────────────

  ImagePreview::ImagePreview() {
    setClipChildren(true);
    auto image = std::make_unique<Image>();
    image->setFit(ImageFit::Stretch);
    image->setHitTestVisible(false);
    m_image = static_cast<Image*>(addChild(std::move(image)));
    auto input = std::make_unique<InputArea>();
    input->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB);
    input->setOnPress([this](const InputArea::PointerData& d) {
      if (!d.pressed) {
        m_dragging = false;
        return;
      }
      if (d.time - m_lastClick < kDoubleClickMs) {
        m_lastClick = 0;
        if (m_fit) {
          // Doble clic: tamaño real centrado en el punto.
          zoomAround(1.0F / scale(), d.localX, d.localY);
        } else {
          fit();
        }
        return;
      }
      m_lastClick = d.time;
      m_dragging = true;
      m_lastX = d.localX;
      m_lastY = d.localY;
    });
    input->setOnMotion([this](const InputArea::PointerData& d) {
      if (!m_dragging || m_input == nullptr || !m_input->pressed()) {
        return;
      }
      const float s = scale();
      m_offsetX -= (d.localX - m_lastX) / s;
      m_offsetY -= (d.localY - m_lastY) / s;
      m_lastX = d.localX;
      m_lastY = d.localY;
      clampOffset();
      markLayoutDirty();
    });
    input->setOnAxisHandler([this](const InputArea::PointerData& d) -> bool {
      if (d.axis != 0 || m_imageWidth <= 0) {
        return false;
      }
      zoomAround(d.axisValue < 0 ? 1.15F : 1.0F / 1.15F, d.localX, d.localY);
      return true;
    });
    m_input = static_cast<InputArea*>(addChild(std::move(input)));
  }

  void ImagePreview::setTexture(TextureHandle texture, int width, int height) {
    m_texture = texture;
    m_imageWidth = width;
    m_imageHeight = height;
    m_fit = true;
    m_offsetX = width * 0.5F;
    m_offsetY = height * 0.5F;
    markLayoutDirty();
  }

  void ImagePreview::setSize(float width, float height) {
    Node::setSize(width, height);
    markLayoutDirty();
  }

  float ImagePreview::fitScale() const {
    if (m_imageWidth <= 0 || m_imageHeight <= 0 || width() <= 0.0F || height() <= 0.0F) {
      return 1.0F;
    }
    // Ajustar sin agrandar fotos chicas por encima de su tamaño real.
    return std::min({width() / m_imageWidth, height() / m_imageHeight, 1.0F});
  }

  float ImagePreview::zoomPercent() const { return scale() * 100.0F; }

  void ImagePreview::notifyZoom() {
    markLayoutDirty();
    if (m_onZoomChanged) {
      m_onZoomChanged();
    }
  }

  void ImagePreview::zoomAround(float factor, float x, float y) {
    const float s = scale();
    const float px = m_offsetX + (x - width() * 0.5F) / s;
    const float py = m_offsetY + (y - height() * 0.5F) / s;
    const float next = std::clamp(s * factor, std::min(fitScale(), 0.05F), kMaxScale);
    m_fit = false;
    m_scale = next;
    m_offsetX = px - (x - width() * 0.5F) / next;
    m_offsetY = py - (y - height() * 0.5F) / next;
    if (next <= fitScale() + 1e-4F) {
      m_fit = true; // al volver al tamaño de ajuste, queda ajustada (sigue al redimensionar)
    }
    clampOffset();
    notifyZoom();
  }

  void ImagePreview::zoomBy(float factor) { zoomAround(factor, width() * 0.5F, height() * 0.5F); }

  void ImagePreview::fit() {
    m_fit = true;
    clampOffset();
    notifyZoom();
  }

  void ImagePreview::actualSize() { zoomAround(1.0F / scale(), width() * 0.5F, height() * 0.5F); }

  void ImagePreview::clampOffset() {
    const float s = scale();
    const auto clampAxis = [s](float& offset, int image, float view) {
      const float half = view * 0.5F / s;
      offset = image * s <= view ? image * 0.5F : std::clamp(offset, half, image - half);
    };
    clampAxis(m_offsetX, m_imageWidth, width());
    clampAxis(m_offsetY, m_imageHeight, height());
  }

  void ImagePreview::doLayout(Renderer& renderer) {
    m_input->setSize(width(), height());
    m_input->setCursorShape(m_fit ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_IN : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB);
    m_image->setVisible(m_texture.valid());
    if (!m_texture.valid()) {
      return;
    }
    clampOffset();
    const float s = scale();
    const float w = m_imageWidth * s;
    const float h = m_imageHeight * s;
    m_image->setExternalTexture(renderer, m_texture);
    m_image->setPosition(std::round(width() * 0.5F - m_offsetX * s), std::round(height() * 0.5F - m_offsetY * s));
    m_image->setSize(std::round(w), std::round(h));
  }

} // namespace okaru::files
