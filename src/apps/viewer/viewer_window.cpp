#include "apps/viewer/viewer_window.h"

#include "apps/common/okaru_app.h"
#include "apps/common/portal_file_chooser.h"
#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "core/process/process.h"
#include "dbus/session_bus.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "system/okaru_system_info.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <format>
#include <gio/gio.h>
#include <linux/input-event-codes.h>
#include <numbers>
#include <sdbus-c++/sdbus-c++.h>
#include <thread>

namespace okaru::viewer {

  namespace {

    namespace fs = std::filesystem;
    using i18n::tr;

    // shortcut: lado máximo fijo en vez de preguntar a la GPU; 16384 lo admiten todas las de escritorio actuales.
    constexpr int kMaxTextureSize = 16384;
    constexpr float kMinZoom = 0.02F;
    constexpr float kMaxZoom = 50.0F;
    constexpr float kZoomStep = 1.25F;
    constexpr std::uint32_t kDoubleClickMs = 400;

    std::string modifiedText(const fs::path& path) {
      std::error_code ec;
      const auto written = fs::last_write_time(path, ec);
      if (ec) {
        return {};
      }
      const auto seconds =
          std::chrono::system_clock::to_time_t(std::chrono::clock_cast<std::chrono::system_clock>(written));
      std::tm local{};
      localtime_r(&seconds, &local);
      char buffer[64];
      return std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &local) > 0 ? std::string(buffer) : std::string();
    }

    std::string contentType(const fs::path& path) {
      gboolean uncertain = FALSE;
      gchar* type = g_content_type_guess(path.c_str(), nullptr, 0, &uncertain);
      gchar* mime = type != nullptr ? g_content_type_get_mime_type(type) : nullptr;
      std::string out = mime != nullptr ? mime : "application/octet-stream";
      g_free(mime);
      g_free(type);
      return out;
    }

    std::string fileUri(const fs::path& path) {
      gchar* uri = g_filename_to_uri(path.c_str(), nullptr, nullptr);
      std::string out = uri != nullptr ? uri : std::string();
      g_free(uri);
      return out;
    }

  } // namespace

  ViewerWindow::ViewerWindow(apps::OkaruApp& app, const std::vector<fs::path>& files) : m_app(app) {
    m_app.setBuild([this](float w, float h, float s) { return build(w, h, s); });
    m_app.setKeyHandler([this](const KeyboardEvent& event) { return onKey(event); });
    openFiles(files);
  }

  ViewerWindow::~ViewerWindow() {
    // Un hilo de decodificación que termine después ya no encuentra a quién entregar.
    ++*m_generation;
  }

  // ── Archivos y decodificación ───────────────────────────────────────────────────────────────────────

  void ViewerWindow::openFiles(const std::vector<fs::path>& files) {
    m_files.clear();
    m_index = 0;
    std::error_code ec;
    if (files.size() == 1 && fs::is_directory(files.front(), ec)) {
      // Una carpeta: sus imágenes, desde la primera.
      for (const auto& entry : fs::directory_iterator(files.front(), ec)) {
        if (entry.is_regular_file(ec) && isSupportedImage(entry.path())) {
          m_files = siblingImages(entry.path());
          break;
        }
      }
    } else if (files.size() == 1) {
      // Un archivo: se recorre su carpeta, como el visor de GNOME.
      const fs::path file = fs::absolute(files.front(), ec);
      m_files = siblingImages(file);
      const auto it = std::ranges::find(m_files, file);
      if (it == m_files.end()) {
        m_files.insert(m_files.begin(), file); // extensión desconocida: se intenta igual
      } else {
        m_index = static_cast<std::size_t>(it - m_files.begin());
      }
    } else {
      for (const auto& file : files) {
        if (fs::is_regular_file(file, ec)) {
          m_files.push_back(fs::absolute(file, ec));
        }
      }
    }
    if (m_files.empty()) {
      m_error.clear();
      unloadTextures();
      m_app.requestRebuild();
      return;
    }
    show(m_index);
  }

  void ViewerWindow::show(std::size_t index) {
    m_index = index;
    m_fitMode = true;
    m_panX = 0.0F;
    m_panY = 0.0F;
    m_quarterTurns = 0;
    m_frameTimer.stop();
    m_svgTimer.stop();
    decode(0);
  }

  void ViewerWindow::decode(int svgSize) {
    if (m_index >= m_files.size()) {
      return;
    }
    const unsigned generation = ++*m_generation;
    const fs::path path = m_files[m_index];
    const bool refresh = svgSize > 0; // SVG más grande para el zoom: se conserva la vista
    m_loading = !refresh;
    if (!refresh) {
      m_app.requestRebuild();
    }
    std::thread([this, generation, counter = m_generation, path, svgSize, refresh] {
      auto decoded =
          std::make_shared<std::expected<DecodedImage, std::string>>(decodeImage(path, kMaxTextureSize, svgSize));
      auto camera =
          std::make_shared<std::vector<MetadataField>>(refresh ? std::vector<MetadataField>{} : cameraMetadata(path));
      if (*counter != generation) {
        return;
      }
      DeferredCall::callLater([this, generation, counter, decoded, camera, refresh] {
        if (*counter != generation) {
          return;
        }
        m_loading = false;
        if (!*decoded) {
          if (!refresh) {
            m_error = decoded->error();
            unloadTextures();
          }
        } else {
          m_error.clear();
          m_decoded = std::make_unique<DecodedImage>(std::move(**decoded));
          if (refresh) {
            // Un SVG rasterizado más grande informa ese tamaño; el zoom se mide contra el original.
            m_decoded->sourceWidth = m_sourceWidth;
            m_decoded->sourceHeight = m_sourceHeight;
          } else {
            m_camera = std::move(*camera);
          }
        }
        m_app.requestRebuild();
      });
    }).detach();
  }

  void ViewerWindow::uploadPending(Renderer& renderer) {
    if (m_decoded == nullptr) {
      return;
    }
    unloadTextures();
    auto& textures = renderer.textureManager();
    const bool animated = m_decoded->frames.size() > 1;
    for (const auto& frame : m_decoded->frames) {
      // Mipmaps para una imagen fija: al alejar no se ve "dentada". Las animaciones cambian de cuadro: sin ellos.
      TextureHandle handle = textures.loadFromRgba(frame.rgba.data(), m_decoded->width, m_decoded->height, !animated);
      if (!handle.valid()) {
        unloadTextures();
        m_error = "GPU";
        m_decoded.reset();
        return;
      }
      m_textures.push_back(handle);
      m_durations.push_back(frame.durationMs);
    }
    m_textureWidth = m_decoded->width;
    m_textureHeight = m_decoded->height;
    m_sourceWidth = m_decoded->sourceWidth;
    m_sourceHeight = m_decoded->sourceHeight;
    m_vector = m_decoded->vector;
    m_decoded.reset();
    m_frame = 0;
    startAnimation();
  }

  void ViewerWindow::unloadTextures() {
    m_frameTimer.stop();
    if (Renderer* renderer = m_app.renderer(); renderer != nullptr) {
      for (auto& handle : m_textures) {
        renderer->textureManager().unload(handle);
      }
    }
    m_textures.clear();
    m_durations.clear();
    m_image = nullptr;
  }

  void ViewerWindow::startAnimation() {
    if (m_textures.size() <= 1) {
      return;
    }
    m_frameTimer.start(std::chrono::milliseconds(m_durations[m_frame]), [this] {
      m_frame = (m_frame + 1) % m_textures.size();
      if (m_image != nullptr && m_app.renderer() != nullptr) {
        m_image->setExternalTexture(*m_app.renderer(), m_textures[m_frame]);
        m_app.invalidate();
      }
      startAnimation();
    });
  }

  // ── Geometría ──────────────────────────────────────────────────────────────────────────────────────

  float ViewerWindow::fitZoom() const {
    if (m_sourceWidth <= 0 || m_sourceHeight <= 0 || m_canvasWidth <= 0.0F || m_canvasHeight <= 0.0F) {
      return 1.0F;
    }
    const bool sideways = m_quarterTurns % 2 != 0;
    const auto w = static_cast<float>(sideways ? m_sourceHeight : m_sourceWidth);
    const auto h = static_cast<float>(sideways ? m_sourceWidth : m_sourceHeight);
    const float fit = std::min(m_canvasWidth / w, m_canvasHeight / h);
    // Una imagen pequeña se ve a su tamaño real; un SVG se puede agrandar sin perder calidad.
    return m_vector ? fit : std::min(fit, 1.0F);
  }

  float ViewerWindow::currentZoom() const { return m_fitMode ? fitZoom() : m_zoom; }

  void ViewerWindow::clampPan() {
    const float zoom = currentZoom();
    const bool sideways = m_quarterTurns % 2 != 0;
    const float w = static_cast<float>(sideways ? m_sourceHeight : m_sourceWidth) * zoom;
    const float h = static_cast<float>(sideways ? m_sourceWidth : m_sourceHeight) * zoom;
    const float maxX = std::max(0.0F, (w - m_canvasWidth) * 0.5F);
    const float maxY = std::max(0.0F, (h - m_canvasHeight) * 0.5F);
    m_panX = std::clamp(m_panX, -maxX, maxX);
    m_panY = std::clamp(m_panY, -maxY, maxY);
  }

  void ViewerWindow::layoutImage() {
    if (m_image == nullptr) {
      return;
    }
    clampPan();
    const float zoom = currentZoom();
    const float w = static_cast<float>(m_sourceWidth) * zoom;
    const float h = static_cast<float>(m_sourceHeight) * zoom;
    m_image->setSize(w, h);
    m_image->setPosition(
        std::round(m_canvasWidth * 0.5F + m_panX - w * 0.5F), std::round(m_canvasHeight * 0.5F + m_panY - h * 0.5F)
    );
    m_image->setTransformOrigin(w * 0.5F, h * 0.5F);
    m_image->setRotation(static_cast<float>(m_quarterTurns) * std::numbers::pi_v<float> * 0.5F);
    m_app.invalidate();
  }

  void ViewerWindow::setZoom(float zoom, float anchorX, float anchorY) {
    const float old = currentZoom();
    const float fitLevel = fitZoom();
    zoom = std::clamp(zoom, std::min(kMinZoom, fitLevel), kMaxZoom);
    // Como el visor de GNOME: alejar más que "ajustar" no tiene sentido; vuelve a ajustar.
    if (zoom <= fitLevel * 1.001F) {
      fit();
      return;
    }
    // El punto bajo el puntero se queda quieto.
    m_panX = anchorX - (anchorX - m_panX) * (zoom / old);
    m_panY = anchorY - (anchorY - m_panY) * (zoom / old);
    m_zoom = zoom;
    m_fitMode = false;
    layoutImage();
    maybeRerasterizeSvg();
    m_app.requestRebuild(); // el porcentaje de la cabecera
  }

  void ViewerWindow::zoomBy(float factor, float anchorX, float anchorY) {
    setZoom(currentZoom() * factor, anchorX, anchorY);
  }

  void ViewerWindow::fit() {
    m_fitMode = true;
    m_panX = 0.0F;
    m_panY = 0.0F;
    layoutImage();
    maybeRerasterizeSvg();
    m_app.requestRebuild();
  }

  void ViewerWindow::maybeRerasterizeSvg() {
    if (!m_vector || m_sourceWidth <= 0) {
      return;
    }
    const float needed = static_cast<float>(std::max(m_sourceWidth, m_sourceHeight)) * currentZoom();
    if (needed <= static_cast<float>(std::max(m_textureWidth, m_textureHeight)) * 1.05F) {
      return;
    }
    // Se espera a que termine el gesto de zoom; se redondea a 512 para no rasterizar en cada paso.
    const int size = std::min(kMaxTextureSize, (static_cast<int>(needed) / 512 + 1) * 512);
    m_svgTimer.start(std::chrono::milliseconds(150), [this, size] { decode(size); });
  }

  // ── Escena ─────────────────────────────────────────────────────────────────────────────────────────

  std::unique_ptr<Node> ViewerWindow::build(float width, float height, float scale) {
    m_image = nullptr;
    m_canvas = nullptr;
    m_canvasInput = nullptr;
    if (Renderer* renderer = m_app.renderer(); renderer != nullptr) {
      uploadPending(*renderer);
    }

    const bool fullscreen = m_app.fullscreen();
    const float headerHeight = fullscreen ? 0.0F : 56.0F * scale;
    const bool properties = m_showProperties && !fullscreen && !m_textures.empty();
    const float propertiesWidth = properties ? std::min(320.0F * scale, width * 0.45F) : 0.0F;

    auto root = ui::node({.width = width, .height = height});
    root->addChild(
        ui::box({
            .fill = colorSpecFromRole(fullscreen ? ColorRole::Shadow : ColorRole::Surface),
            .width = width,
            .height = height,
        })
    );
    auto canvas = buildCanvas(width - propertiesWidth, height - headerHeight, scale);
    canvas->setPosition(0.0F, headerHeight);
    root->addChild(std::move(canvas));
    if (!fullscreen) {
      Node* header = root->addChild(buildHeader(width, scale));
      m_app.setDragNode(header);
    }
    if (properties) {
      auto panel = buildProperties(propertiesWidth, height - headerHeight, scale);
      panel->setPosition(width - propertiesWidth, headerHeight);
      root->addChild(std::move(panel));
    }
    if (!m_status.empty()) {
      auto toast = ui::row(
          {.align = FlexAlign::Center,
           .padding = Style::spaceSm * scale,
           .fill = colorSpecFromRole(ColorRole::SurfaceContainerHighest),
           .radius = Style::radiusMd * scale},
          ui::label({.text = m_status, .fontSize = Style::fontSizeBody * scale})
      );
      toast->layout(*m_app.renderer());
      toast->setPosition(
          std::round(((width - propertiesWidth) - toast->width()) * 0.5F),
          height - toast->height() - Style::spaceLg * scale
      );
      root->addChild(std::move(toast));
    }
    layoutImage();
    maybeRerasterizeSvg(); // un SVG pequeño ajustado a la ventana se pide ya a su tamaño en pantalla
    return root;
  }

  std::unique_ptr<Node> ViewerWindow::buildHeader(float width, float scale) {
    const bool hasImage = !m_textures.empty();
    const auto iconButton = [&](const char* glyph, std::string tooltip, bool enabled, std::function<void()> onClick) {
      return ui::button({
          .glyph = glyph,
          .enabled = enabled,
          .variant = ButtonVariant::Ghost,
          .tooltip = std::move(tooltip),
          .onClick = std::move(onClick),
      });
    };

    auto header = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceXs * scale,
        .paddingH = Style::spaceMd * scale,
        .width = width,
        .height = 56.0F * scale,
    });
    header->addChild(iconButton("folder-open", tr("apps.view.open"), true, [this] { openDialog(); }));
    header->addChild(iconButton("chevron-left", tr("apps.view.previous"), m_index > 0, [this] { step(-1); }));
    header->addChild(iconButton("chevron-right", tr("apps.view.next"), m_index + 1 < m_files.size(), [this] {
      step(1);
    }));

    auto titles = ui::column({.align = FlexAlign::Start, .gap = 0.0F, .flexGrow = 1.0F});
    titles->addChild(
        ui::label({
            .text = m_files.empty() ? tr("apps.view.title") : m_files[m_index].filename().string(),
            .fontSize = Style::fontSizeBody * scale,
            .fontWeight = FontWeight::SemiBold,
            .maxLines = 1,
            .ellipsize = TextEllipsize::Middle,
        })
    );
    if (m_files.size() > 1) {
      titles->addChild(
          ui::label({
              .text = std::format("{} / {}", m_index + 1, m_files.size()),
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      );
    }
    header->addChild(std::move(titles));

    header->addChild(iconButton("zoom-out", tr("apps.view.zoom-out"), hasImage, [this] {
      zoomBy(1.0F / kZoomStep, 0, 0);
    }));
    header->addChild(
        ui::button({
            .text = std::format("{}%", static_cast<int>(std::lround(currentZoom() * 100.0F))),
            .fontSize = Style::fontSizeCaption * scale,
            .enabled = hasImage,
            .variant = ButtonVariant::Ghost,
            .tooltip = m_fitMode ? tr("apps.view.zoom-original") : tr("apps.view.zoom-fit"),
            .minWidth = 64.0F * scale,
            .onClick = [this] {
              if (m_fitMode) {
                setZoom(1.0F, 0.0F, 0.0F);
              } else {
                fit();
              }
            },
        })
    );
    header->addChild(iconButton("zoom-in", tr("apps.view.zoom-in"), hasImage, [this] { zoomBy(kZoomStep, 0, 0); }));
    header->addChild(iconButton("rotate", tr("apps.view.rotate-left"), hasImage, [this] { rotate(-1); }));
    header->addChild(iconButton("rotate-clockwise", tr("apps.view.rotate-right"), hasImage, [this] { rotate(1); }));
    header->addChild(iconButton("copy", tr("apps.view.copy"), hasImage, [this] { copyImage(); }));
    header->addChild(iconButton("wallpaper", tr("apps.view.wallpaper"), hasImage, [this] { setWallpaper(); }));
    header->addChild(iconButton("trash", tr("apps.view.trash"), hasImage, [this] { moveToTrash(); }));
    header->addChild(iconButton("info-circle", tr("apps.view.properties.title"), hasImage, [this] {
      m_showProperties = !m_showProperties;
      m_app.requestRebuild();
    }));
    header->addChild(iconButton("maximize", tr("apps.view.fullscreen"), true, [this] { toggleFullscreen(); }));
    header->addChild(iconButton("x", tr("apps.view.close"), true, [this] { m_app.quit(); }));
    return header;
  }

  std::unique_ptr<Node> ViewerWindow::buildCanvas(float width, float height, float scale) {
    m_canvasWidth = width;
    m_canvasHeight = height;
    auto canvas = ui::node({.width = width, .height = height, .clipChildren = true});
    m_canvas = canvas.get();

    if (!m_textures.empty() && m_error.empty()) {
      auto image = ui::image({.fit = ImageFit::Stretch});
      m_image = image.get();
      if (Renderer* renderer = m_app.renderer(); renderer != nullptr) {
        m_image->setExternalTexture(*renderer, m_textures[std::min(m_frame, m_textures.size() - 1)]);
      }
      canvas->addChild(std::move(image));
    } else {
      // Sin imagen: cargando, error o la portada para abrir una.
      auto message = ui::column(
          {.align = FlexAlign::Center,
           .justify = FlexJustify::Center,
           .gap = Style::spaceMd * scale,
           .width = width,
           .height = height}
      );
      if (m_loading) {
        message->addChild(ui::spinner({.spinnerSize = 32.0F * scale}));
      } else {
        message->addChild(
            ui::glyph({
                .glyph = m_error.empty() ? "photo" : "photo-off",
                .glyphSize = 64.0F * scale,
                .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            })
        );
        message->addChild(
            ui::label({
                .text = m_error.empty() ? tr("apps.view.empty.title") : tr("apps.view.error"),
                .fontSize = Style::fontSizeTitle * scale,
                .fontWeight = FontWeight::SemiBold,
            })
        );
        message->addChild(
            ui::label({
                .text = m_error.empty() ? tr("apps.view.empty.hint") : m_error,
                .fontSize = Style::fontSizeBody * scale,
                .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                .maxWidth = width * 0.8F,
                .maxLines = 3,
                .textAlign = TextAlign::Center,
            })
        );
        if (m_files.empty()) {
          message->addChild(
              ui::button({
                  .text = tr("apps.view.open"),
                  .glyph = "folder-open",
                  .variant = ButtonVariant::Primary,
                  .onClick = [this] { openDialog(); },
              })
          );
        }
      }
      canvas->addChild(std::move(message));
      return canvas;
    }

    // Arrastrar mueve, la rueda acerca (bajo el puntero), doble clic alterna ajustar / tamaño real.
    canvas->addChild(
        ui::inputArea({
            .out = &m_canvasInput,
            .acceptedButtons = BTN_LEFT,
            .width = width,
            .height = height,
            .zIndex = 1,
            .onMotion =
                [this](const InputArea::PointerData& data) {
                  if (!m_dragging || m_canvasInput == nullptr) {
                    return;
                  }
                  if (!m_canvasInput->pressed()) {
                    m_dragging = false;
                    return;
                  }
                  m_panX = m_dragPanX + (data.localX - m_dragStartX);
                  m_panY = m_dragPanY + (data.localY - m_dragStartY);
                  layoutImage();
                },
            .onPress =
                [this](const InputArea::PointerData& data) {
                  if (!data.pressed) {
                    m_dragging = false;
                    return;
                  }
                  m_dragging = true;
                  m_dragStartX = data.localX;
                  m_dragStartY = data.localY;
                  m_dragPanX = m_panX;
                  m_dragPanY = m_panY;
                },
            .onClick =
                [this](const InputArea::PointerData& data) {
                  m_dragging = false;
                  if (data.time - m_lastClickTime < kDoubleClickMs) {
                    m_lastClickTime = 0;
                    if (m_fitMode) {
                      setZoom(1.0F, data.localX - m_canvasWidth * 0.5F, data.localY - m_canvasHeight * 0.5F);
                    } else {
                      fit();
                    }
                    return;
                  }
                  m_lastClickTime = data.time;
                },
            .onAxisHandler = [this](const InputArea::PointerData& data) -> bool {
              if (data.axis != 0) {
                return false; // horizontal
              }
              const float anchorX = data.localX - m_canvasWidth * 0.5F;
              const float anchorY = data.localY - m_canvasHeight * 0.5F;
              // Rueda: zoom. Panel táctil (dos dedos): mover la imagen cuando está ampliada.
              if (data.axisSource == 1 && !m_fitMode) {
                m_panY -= static_cast<float>(data.axisValue);
                layoutImage();
                return true;
              }
              const double steps = data.axisValue120 != 0 ? data.axisValue120 / 120.0 : data.axisValue / 10.0;
              zoomBy(static_cast<float>(std::pow(1.15, -steps)), anchorX, anchorY);
              return true;
            },
        })
    );
    return canvas;
  }

  std::unique_ptr<Node> ViewerWindow::buildProperties(float width, float height, float scale) {
    ScrollView* view = nullptr;
    auto scroll = ui::scrollView({
        .out = &view,
        .viewportPaddingH = Style::spaceLg * scale,
        .viewportPaddingV = Style::spaceMd * scale,
        .fill = colorSpecFromRole(ColorRole::SurfaceContainerLow),
        .width = width,
        .height = height,
    });
    Flex* content = view->content();
    content->setGap(Style::spaceMd * scale);
    content->setAlign(FlexAlign::Stretch);
    const auto row = [&](const std::string& key, std::string value) {
      if (value.empty()) {
        return;
      }
      content->addChild(
          ui::column(
              {.align = FlexAlign::Start, .gap = 2.0F * scale, .fillWidth = true},
              ui::label({
                  .text = tr(std::format("apps.view.properties.{}", key)),
                  .fontSize = Style::fontSizeCaption * scale,
                  .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              }),
              ui::label(
                  {.text = std::move(value),
                   .fontSize = Style::fontSizeBody * scale,
                   .maxWidth = width - 2.0F * Style::spaceLg * scale,
                   .maxLines = 3}
              )
          )
      );
    };

    content->addChild(
        ui::label({
            .text = tr("apps.view.properties.title"),
            .fontSize = Style::fontSizeTitle * scale,
            .fontWeight = FontWeight::SemiBold,
        })
    );
    const fs::path& path = m_files[m_index];
    std::error_code ec;
    row("name", path.filename().string());
    row("folder", path.parent_path().string());
    row("format", formatName(path));
    row("dimensions", std::format("{} × {}", m_sourceWidth, m_sourceHeight));
    if (const auto bytes = fs::file_size(path, ec); !ec) {
      row("size", okaru::sysinfo::formatBytes(bytes));
    }
    row("modified", modifiedText(path));
    if (m_textures.size() > 1) {
      row("frames", std::to_string(m_textures.size()));
    }
    for (const auto& field : m_camera) {
      row(field.key, field.value);
    }
    content->addChild(
        ui::button({
            .text = tr("apps.view.folder"),
            .glyph = "folder",
            .variant = ButtonVariant::Outline,
            .onClick = [this] { showInFolder(); },
        })
    );
    return scroll;
  }

  // ── Acciones ────────────────────────────────────────────────────────────────────────────────────────

  void ViewerWindow::step(int delta) {
    if (m_files.empty()) {
      return;
    }
    const auto next = static_cast<long>(m_index) + delta;
    if (next < 0 || next >= static_cast<long>(m_files.size())) {
      return;
    }
    show(static_cast<std::size_t>(next));
  }

  void ViewerWindow::rotate(int quarterTurns) {
    if (m_textures.empty()) {
      return;
    }
    m_quarterTurns = (m_quarterTurns + quarterTurns + 4) % 4;
    if (m_fitMode) {
      fit();
    } else {
      layoutImage();
    }
  }

  void ViewerWindow::toggleFullscreen() {
    m_app.setFullscreen(!m_app.fullscreen());
    m_app.requestRebuild();
  }

  void ViewerWindow::copyImage() {
    if (m_files.empty()) {
      return;
    }
    // El archivo tal cual, con su tipo: pegarlo en otra app da la imagen original, no una copia recomprimida.
    const fs::path& path = m_files[m_index];
    const bool started =
        process::runAsync({"sh", "-c", "exec wl-copy --type \"$1\" < \"$2\"", "sh", contentType(path), path.string()});
    m_status = started ? tr("apps.view.status.copied") : tr("apps.view.status.failed");
    m_statusTimer.start(std::chrono::seconds(2), [this] {
      m_status.clear();
      m_app.requestRebuild();
    });
    m_app.requestRebuild();
  }

  void ViewerWindow::setWallpaper() {
    if (m_files.empty()) {
      return;
    }
    const bool started = process::runAsync({"okaru", "msg", "wallpaper-set", m_files[m_index].c_str()});
    m_status = started ? tr("apps.view.status.wallpaper") : tr("apps.view.status.failed");
    m_statusTimer.start(std::chrono::seconds(2), [this] {
      m_status.clear();
      m_app.requestRebuild();
    });
    m_app.requestRebuild();
  }

  void ViewerWindow::moveToTrash() {
    if (m_files.empty()) {
      return;
    }
    GFile* file = g_file_new_for_path(m_files[m_index].c_str());
    GError* error = nullptr;
    const bool trashed = g_file_trash(file, nullptr, &error) != FALSE;
    g_object_unref(file);
    g_clear_error(&error);
    m_status = trashed ? tr("apps.view.status.trashed") : tr("apps.view.status.failed");
    m_statusTimer.start(std::chrono::seconds(2), [this] {
      m_status.clear();
      m_app.requestRebuild();
    });
    if (trashed) {
      m_files.erase(m_files.begin() + static_cast<std::ptrdiff_t>(m_index));
      if (m_files.empty()) {
        ++*m_generation;
        unloadTextures();
        m_error.clear();
      } else {
        show(std::min(m_index, m_files.size() - 1));
      }
    }
    m_app.requestRebuild();
  }

  void ViewerWindow::showInFolder() {
    if (m_files.empty()) {
      return;
    }
    // El explorador abre la carpeta con el archivo seleccionado; si no implementa FileManager1, xdg-open.
    if (SessionBus* bus = m_app.sessionBus(); bus != nullptr) {
      try {
        auto proxy = sdbus::createProxy(
            bus->connection(), sdbus::ServiceName{"org.freedesktop.FileManager1"},
            sdbus::ObjectPath{"/org/freedesktop/FileManager1"}
        );
        proxy->callMethod("ShowItems")
            .onInterface("org.freedesktop.FileManager1")
            .withArguments(std::vector<std::string>{fileUri(m_files[m_index])}, std::string());
        return;
      } catch (const sdbus::Error&) {
      }
    }
    (void)process::runAsync({"xdg-open", m_files[m_index].parent_path().c_str()});
  }

  void ViewerWindow::openDialog() {
    SessionBus* bus = m_app.sessionBus();
    if (bus == nullptr) {
      return;
    }
    if (m_chooser == nullptr) {
      m_chooser = std::make_unique<apps::PortalFileChooser>(*bus);
    }
    m_chooser->open(
        tr("apps.view.open"), {{.name = tr("apps.view.filter"), .mimeTypes = {"image/*"}}}, true,
        [this](std::vector<std::string> paths) {
          if (!paths.empty()) {
            openFiles({paths.begin(), paths.end()});
          }
        }
    );
  }

  bool ViewerWindow::onKey(const KeyboardEvent& event) {
    if (!event.pressed || event.preedit) {
      return false;
    }
    const bool ctrl = (event.modifiers & KeyMod::Ctrl) != 0;
    const bool shift = (event.modifiers & KeyMod::Shift) != 0;
    switch (event.sym) {
    case XKB_KEY_q:
    case XKB_KEY_w:
      if (ctrl) {
        m_app.quit();
        return true;
      }
      return false;
    case XKB_KEY_o:
      if (ctrl) {
        openDialog();
        return true;
      }
      return false;
    case XKB_KEY_c:
      if (ctrl) {
        copyImage();
        return true;
      }
      return false;
    case XKB_KEY_Left:
    case XKB_KEY_Page_Up:
    case XKB_KEY_BackSpace:
      step(-1);
      return true;
    case XKB_KEY_Right:
    case XKB_KEY_Page_Down:
    case XKB_KEY_space:
      step(1);
      return true;
    case XKB_KEY_Home:
      step(-static_cast<int>(m_index));
      return true;
    case XKB_KEY_End:
      step(static_cast<int>(m_files.size()) - 1 - static_cast<int>(m_index));
      return true;
    case XKB_KEY_plus:
    case XKB_KEY_equal:
    case XKB_KEY_KP_Add:
      zoomBy(kZoomStep, 0.0F, 0.0F);
      return true;
    case XKB_KEY_minus:
    case XKB_KEY_KP_Subtract:
      zoomBy(1.0F / kZoomStep, 0.0F, 0.0F);
      return true;
    case XKB_KEY_0:
    case XKB_KEY_KP_0:
      fit();
      return true;
    case XKB_KEY_1:
    case XKB_KEY_KP_1:
      setZoom(1.0F, 0.0F, 0.0F);
      return true;
    case XKB_KEY_r:
    case XKB_KEY_R:
      rotate(shift ? -1 : 1);
      return true;
    case XKB_KEY_f:
    case XKB_KEY_F11:
      toggleFullscreen();
      return true;
    case XKB_KEY_i:
      if (!m_textures.empty()) {
        m_showProperties = !m_showProperties;
        m_app.requestRebuild();
      }
      return true;
    case XKB_KEY_Delete:
      moveToTrash();
      return true;
    case XKB_KEY_Escape:
      if (m_app.fullscreen()) {
        toggleFullscreen();
      } else if (m_showProperties) {
        m_showProperties = false;
        m_app.requestRebuild();
      }
      return true;
    default:
      return false;
    }
  }

} // namespace okaru::viewer
