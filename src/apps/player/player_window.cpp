#include "apps/player/player_window.h"

#include "apps/common/okaru_app.h"
#include "apps/common/portal_file_chooser.h"
#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/file_utils.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <linux/input-event-codes.h>

namespace okaru::player {

  namespace {

    namespace fs = std::filesystem;
    using i18n::tr;

    constexpr auto kHideControlsAfter = std::chrono::milliseconds(2500);
    constexpr std::uint32_t kDoubleClickMs = 300;
    constexpr std::array<double, 7> kSpeeds{0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0};

    // mpv cambia estado de GL que el renderer de OKARU da por fijo (alineación y largo de fila al subir píxeles,
    // mezcla, VAO, programa…): se guarda antes de que dibuje y se deja exactamente igual después.
    class GlStateGuard {
    public:
      GlStateGuard() {
        for (std::size_t i = 0; i < kInts.size(); ++i) {
          glGetIntegerv(kInts[i], &m_ints[i]);
        }
        for (std::size_t i = 0; i < kCaps.size(); ++i) {
          m_caps[i] = glIsEnabled(kCaps[i]);
        }
        glGetIntegerv(GL_VIEWPORT, m_viewport.data());
        glGetIntegerv(GL_SCISSOR_BOX, m_scissor.data());
        glGetBooleanv(GL_COLOR_WRITEMASK, m_colorMask.data());
        // Y mpv recibe el estado por defecto de GL, que es el que asume (p. ej. al subir cuadros NV12 de la GPU).
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glPixelStorei(kUnpackRowLength, 0);
        glPixelStorei(kUnpackSkipRows, 0);
        glPixelStorei(kUnpackSkipPixels, 0);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);
      }

      ~GlStateGuard() {
        using BindVertexArray = void (*)(GLuint);
        static const auto bindVertexArray = reinterpret_cast<BindVertexArray>(eglGetProcAddress("glBindVertexArray"));
        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(value(GL_FRAMEBUFFER_BINDING)));
        if (bindVertexArray != nullptr) {
          bindVertexArray(static_cast<GLuint>(value(kVertexArrayBinding)));
        }
        glUseProgram(static_cast<GLuint>(value(GL_CURRENT_PROGRAM)));
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(value(GL_ARRAY_BUFFER_BINDING)));
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLuint>(value(GL_ELEMENT_ARRAY_BUFFER_BINDING)));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(value(GL_TEXTURE_BINDING_2D)));
        glActiveTexture(static_cast<GLenum>(value(GL_ACTIVE_TEXTURE)));
        glPixelStorei(GL_UNPACK_ALIGNMENT, value(GL_UNPACK_ALIGNMENT));
        glPixelStorei(GL_PACK_ALIGNMENT, value(GL_PACK_ALIGNMENT));
        glPixelStorei(kUnpackRowLength, value(kUnpackRowLength));
        glPixelStorei(kUnpackSkipRows, value(kUnpackSkipRows));
        glPixelStorei(kUnpackSkipPixels, value(kUnpackSkipPixels));
        glBlendFuncSeparate(
            static_cast<GLenum>(value(GL_BLEND_SRC_RGB)), static_cast<GLenum>(value(GL_BLEND_DST_RGB)),
            static_cast<GLenum>(value(GL_BLEND_SRC_ALPHA)), static_cast<GLenum>(value(GL_BLEND_DST_ALPHA))
        );
        glBlendEquationSeparate(
            static_cast<GLenum>(value(GL_BLEND_EQUATION_RGB)), static_cast<GLenum>(value(GL_BLEND_EQUATION_ALPHA))
        );
        for (std::size_t i = 0; i < kCaps.size(); ++i) {
          if (m_caps[i] != 0) {
            glEnable(kCaps[i]);
          } else {
            glDisable(kCaps[i]);
          }
        }
        glViewport(m_viewport[0], m_viewport[1], m_viewport[2], m_viewport[3]);
        glScissor(m_scissor[0], m_scissor[1], m_scissor[2], m_scissor[3]);
        glColorMask(m_colorMask[0], m_colorMask[1], m_colorMask[2], m_colorMask[3]);
      }

      GlStateGuard(const GlStateGuard&) = delete;
      GlStateGuard& operator=(const GlStateGuard&) = delete;

    private:
      // Enumeraciones de GLES 3 que GLES2/gl2.h no declara.
      static constexpr GLenum kVertexArrayBinding = 0x85B5;
      static constexpr GLenum kUnpackRowLength = 0x0CF2;
      static constexpr GLenum kUnpackSkipRows = 0x0CF3;
      static constexpr GLenum kUnpackSkipPixels = 0x0CF4;
      static constexpr std::array<GLenum, 17> kInts{
          GL_FRAMEBUFFER_BINDING,
          kVertexArrayBinding,
          GL_CURRENT_PROGRAM,
          GL_ARRAY_BUFFER_BINDING,
          GL_ELEMENT_ARRAY_BUFFER_BINDING,
          GL_ACTIVE_TEXTURE,
          GL_TEXTURE_BINDING_2D,
          GL_UNPACK_ALIGNMENT,
          GL_PACK_ALIGNMENT,
          kUnpackRowLength,
          kUnpackSkipRows,
          kUnpackSkipPixels,
          GL_BLEND_SRC_RGB,
          GL_BLEND_DST_RGB,
          GL_BLEND_SRC_ALPHA,
          GL_BLEND_DST_ALPHA,
          GL_BLEND_EQUATION_RGB,
      };
      static constexpr std::array<GLenum, 6> kCaps{GL_BLEND,        GL_SCISSOR_TEST, GL_DEPTH_TEST,
                                                   GL_STENCIL_TEST, GL_CULL_FACE,    GL_DITHER};

      [[nodiscard]] GLint value(GLenum name) const {
        if (name == GL_BLEND_EQUATION_ALPHA) {
          return m_blendEquationAlpha;
        }
        for (std::size_t i = 0; i < kInts.size(); ++i) {
          if (kInts[i] == name) {
            return m_ints[i];
          }
        }
        return 0;
      }

      std::array<GLint, kInts.size()> m_ints{};
      GLint m_blendEquationAlpha = [] {
        GLint v = GL_FUNC_ADD;
        glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &v);
        return v;
      }();
      std::array<GLboolean, kCaps.size()> m_caps{};
      std::array<GLint, 4> m_viewport{};
      std::array<GLint, 4> m_scissor{};
      std::array<GLboolean, 4> m_colorMask{};
    };

    std::string trackName(const Track& track, std::size_t index) {
      if (!track.title.empty() && !track.lang.empty()) {
        return std::format("{} ({})", track.title, track.lang);
      }
      if (!track.title.empty()) {
        return track.title;
      }
      if (!track.lang.empty()) {
        return track.lang;
      }
      return tr("apps.play.track", "number", index + 1);
    }

  } // namespace

  PlayerWindow::PlayerWindow(apps::OkaruApp& app, const std::vector<fs::path>& files)
      : m_app(app), m_player(fs::path(FileUtils::stateDir()) / "player-watch-later") {
    m_player.setOnChange([this](bool structural) { onPlayerChange(structural); });
    m_player.setOnFrame([this] { m_app.requestFrame(); });
    m_app.setBuild([this](float w, float h, float s) { return build(w, h, s); });
    m_app.setKeyHandler([this](const KeyboardEvent& event) { return onKey(event); });
    m_app.setBeforeRender([this](Renderer& renderer) { beforeRender(renderer); });
    m_app.setOnClose([this] {
      m_player.freeRender();
      m_frame.destroy();
    });
    openFiles(files);
  }

  PlayerWindow::~PlayerWindow() = default;

  // ── Lista ───────────────────────────────────────────────────────────────────────────────────────────

  void PlayerWindow::openFiles(const std::vector<fs::path>& files) {
    m_files.clear();
    m_index = 0;
    std::error_code ec;
    if (files.size() == 1 && files.front().string().find("://") != std::string::npos) {
      m_files = files; // una URL (mpv la abre si tiene con qué: http, yt-dlp…)
    } else if (files.size() == 1 && fs::is_directory(files.front(), ec)) {
      for (const auto& entry : fs::directory_iterator(files.front(), ec)) {
        if (entry.is_regular_file(ec) && isMediaFile(entry.path())) {
          m_files = siblingMedia(entry.path());
          break;
        }
      }
    } else if (files.size() == 1) {
      // Un archivo: el resto de la carpeta queda como lista, como en un explorador.
      const fs::path file = fs::absolute(files.front(), ec);
      m_files = siblingMedia(file);
      const auto it = std::ranges::find(m_files, file);
      if (it == m_files.end()) {
        m_files.insert(m_files.begin(), file);
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
    if (!m_files.empty()) {
      play(m_index);
    }
    m_app.requestRebuild();
  }

  void PlayerWindow::play(std::size_t index) {
    m_index = index;
    if (!m_renderReady) {
      m_pendingPlay = index;
      m_app.requestRebuild();
      return;
    }
    m_player.load(m_files[index].string());
    wake();
    m_app.requestRebuild();
  }

  void PlayerWindow::step(int delta) {
    const auto next = static_cast<long>(m_index) + delta;
    if (next >= 0 && next < static_cast<long>(m_files.size())) {
      play(static_cast<std::size_t>(next));
    }
  }

  void PlayerWindow::onPlayerChange(bool structural) {
    const auto& state = m_player.state();
    // Al terminar un archivo sigue el próximo de la lista.
    if (state.ended && state.loaded && m_index + 1 < m_files.size()) {
      step(1);
      return;
    }
    if (structural) {
      m_app.requestRebuild();
    } else {
      updateTimeline();
    }
  }

  void PlayerWindow::updateTimeline() {
    const auto& state = m_player.state();
    if (m_timeline != nullptr && !m_timeline->dragging()) {
      m_syncingTimeline = true;
      m_timeline->setRange(0.0, std::max(state.duration, 0.001));
      m_timeline->setValue(state.position);
      m_syncingTimeline = false;
    }
    if (m_positionLabel != nullptr) {
      m_positionLabel->setText(formatTime(state.position));
    }
    if (m_remainingLabel != nullptr) {
      m_remainingLabel->setText("−" + formatTime(state.duration - state.position));
    }
    m_app.invalidate();
  }

  void PlayerWindow::wake() {
    if (!m_controlsVisible) {
      m_controlsVisible = true;
      m_app.requestRebuild();
    }
    m_hideTimer.start(kHideControlsAfter, [this] {
      const auto& state = m_player.state();
      // Solo se ocultan sobre un video que avanza; en pausa, con audio o con el puntero encima se quedan.
      if (!state.paused && state.hasVideo() && !m_overControls) {
        m_controlsVisible = false;
        m_app.requestRebuild();
      }
    });
  }

  void PlayerWindow::toggleFullscreen() {
    m_app.setFullscreen(!m_app.fullscreen());
    wake();
    m_app.requestRebuild();
  }

  void PlayerWindow::openDialog() {
    SessionBus* bus = m_app.sessionBus();
    if (bus == nullptr) {
      return;
    }
    if (m_chooser == nullptr) {
      m_chooser = std::make_unique<apps::PortalFileChooser>(*bus);
    }
    m_chooser->open(
        tr("apps.play.open"), {{.name = tr("apps.play.filter"), .mimeTypes = {"video/*", "audio/*"}}}, true,
        [this](std::vector<std::string> paths) {
          if (!paths.empty()) {
            openFiles({paths.begin(), paths.end()});
          }
        }
    );
  }

  // ── Video ───────────────────────────────────────────────────────────────────────────────────────────

  void PlayerWindow::beforeRender(Renderer& renderer) {
    if (!m_player.valid()) {
      return;
    }
    if (!m_renderReady) {
      {
        const GlStateGuard guard;
        m_renderReady = m_player.initRender(m_app.wlDisplay());
      }
      if (m_renderReady && m_pendingPlay.has_value()) {
        const std::size_t index = *m_pendingPlay;
        m_pendingPlay.reset();
        play(index);
      }
    }
    if (!m_renderReady || m_video == nullptr || m_canvasWidth <= 0.0F) {
      return;
    }
    const auto width = static_cast<std::uint32_t>(std::ceil(m_canvasWidth * m_app.bufferScale()));
    const auto height = static_cast<std::uint32_t>(std::ceil(m_canvasHeight * m_app.bufferScale()));
    const GlStateGuard guard;
    if (!m_frame.valid() || m_frame.width() != width || m_frame.height() != height) {
      if (!m_frame.create(renderer.textureManager(), width, height)) {
        return;
      }
      m_video->setExternalTexture(renderer, m_frame.colorHandle());
      m_forceRender = true;
    }
    if (m_player.renderIfNeeded(m_frame.id(), static_cast<int>(width), static_cast<int>(height), m_forceRender)) {
      m_forceRender = false;
      m_video->markPaintDirty();
    }
  }

  // ── Escena ─────────────────────────────────────────────────────────────────────────────────────────

  std::unique_ptr<Node> PlayerWindow::build(float width, float height, float scale) {
    m_video = nullptr;
    m_timeline = nullptr;
    m_positionLabel = nullptr;
    m_remainingLabel = nullptr;
    const auto& state = m_player.state();
    const bool fullscreen = m_app.fullscreen();
    const float headerHeight = fullscreen ? 0.0F : 56.0F * scale;
    m_canvasWidth = width;
    m_canvasHeight = height - headerHeight;

    auto root = ui::node({.width = width, .height = height});
    root->addChild(ui::box({.fill = colorSpecFromRole(ColorRole::Surface), .width = width, .height = height}));
    if (!fullscreen) {
      Node* header = root->addChild(buildHeader(width, scale));
      m_app.setDragNode(header);
    }

    auto canvas = ui::node({.width = m_canvasWidth, .height = m_canvasHeight, .clipChildren = true});
    canvas->setPosition(0.0F, headerHeight);
    const bool video = state.hasVideo();
    if (video) {
      canvas->addChild(
          ui::box({.fill = fixedColorSpec(rgba(0, 0, 0, 1)), .width = m_canvasWidth, .height = m_canvasHeight})
      );
      auto image = ui::image({.fit = ImageFit::Stretch, .width = m_canvasWidth, .height = m_canvasHeight});
      m_video = image.get();
      if (m_frame.valid() && m_app.renderer() != nullptr) {
        m_video->setExternalTexture(*m_app.renderer(), m_frame.colorHandle());
      }
      canvas->addChild(std::move(image));
    } else {
      canvas->addChild(buildAudioCover(m_canvasWidth, m_canvasHeight, scale));
    }

    // Clic: pausa. Doble clic: pantalla completa. Mover: muestra los controles. Rueda: volumen.
    // Sin nada cargado no se agrega: taparía el botón «Abrir…» de la portada vacía.
    if (!m_files.empty()) {
      canvas->addChild(
          ui::inputArea({
              .acceptedButtons = BTN_LEFT,
              .width = m_canvasWidth,
              .height = m_canvasHeight,
              .zIndex = 1,
              .onMotion =
                  [this](const InputArea::PointerData& data) {
                    if (data.sceneX != m_pointerX || data.sceneY != m_pointerY) {
                      m_pointerX = data.sceneX;
                      m_pointerY = data.sceneY;
                      wake();
                    }
                  },
              .onClick =
                  [this](const InputArea::PointerData& data) {
                    if (m_files.empty()) {
                      return;
                    }
                    if (data.time - m_lastClickTime < kDoubleClickMs) {
                      m_lastClickTime = 0;
                      m_clickTimer.stop();
                      toggleFullscreen();
                      return;
                    }
                    m_lastClickTime = data.time;
                    m_clickTimer.start(std::chrono::milliseconds(kDoubleClickMs), [this] {
                      m_player.togglePause();
                      wake();
                    });
                  },
              .onAxisHandler = [this](const InputArea::PointerData& data) -> bool {
                if (data.axis != 0) {
                  return false;
                }
                const double steps = data.axisValue120 != 0 ? data.axisValue120 / 120.0 : data.axisValue / 10.0;
                m_player.setVolume(m_player.state().volume - steps * 5.0);
                wake();
                return true;
              },
          })
      );
    }

    if (!m_files.empty() && (m_controlsVisible || state.paused || !video)) {
      auto controls = buildControls(m_canvasWidth, scale);
      controls->setPosition(0.0F, m_canvasHeight - controls->height());
      controls->setZIndex(2);
      canvas->addChild(std::move(controls));
    }
    root->addChild(std::move(canvas));
    updateTimeline();
    return root;
  }

  std::unique_ptr<Node> PlayerWindow::buildHeader(float width, float scale) {
    const auto& state = m_player.state();
    auto header = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceXs * scale,
        .paddingH = Style::spaceMd * scale,
        .width = width,
        .height = 56.0F * scale,
    });
    header->addChild(
        ui::button({
            .glyph = "folder-open",
            .variant = ButtonVariant::Ghost,
            .tooltip = tr("apps.play.open"),
            .onClick = [this] { openDialog(); },
        })
    );
    auto titles = ui::column({.align = FlexAlign::Start, .gap = 0.0F, .flexGrow = 1.0F});
    std::string title = tr("apps.play.title");
    if (!m_files.empty()) {
      title = !state.title.empty() ? state.title : m_files[m_index].filename().string();
    }
    titles->addChild(
        ui::label({
            .text = std::move(title),
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
    header->addChild(
        ui::button({
            .glyph = "x",
            .variant = ButtonVariant::Ghost,
            .tooltip = tr("apps.play.close"),
            .onClick = [this] { m_app.quit(); },
        })
    );
    return header;
  }

  std::unique_ptr<Node> PlayerWindow::buildControls(float width, float scale) {
    const auto& state = m_player.state();
    const auto ghost = [&](const char* glyph, std::string tooltip, bool enabled, std::function<void()> onClick) {
      return ui::button({
          .glyph = glyph,
          .enabled = enabled,
          .variant = ButtonVariant::Ghost,
          .tooltip = std::move(tooltip),
          .onClick = std::move(onClick),
      });
    };

    auto panel = ui::column({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceXs * scale,
        .paddingV = Style::spaceSm * scale,
        .paddingH = Style::spaceMd * scale,
        .fill = colorSpecFromRole(ColorRole::SurfaceContainer, 0.9F),
        .width = width,
    });

    // Tiempo: transcurrido, barra y restante.
    panel->addChild(
        ui::row(
            {.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true},
            ui::label({
                .out = &m_positionLabel,
                .text = formatTime(state.position),
                .fontSize = Style::fontSizeCaption * scale,
                .minWidth = 52.0F * scale,
            }),
            ui::slider({
                .out = &m_timeline,
                .minValue = 0.0,
                .maxValue = std::max(state.duration, 0.001),
                .value = state.position,
                .enabled = state.duration > 0.0,
                .wheelAdjustEnabled = false,
                .flexGrow = 1.0F,
                .onValueChanged =
                    [this](double value) {
                      if (m_syncingTimeline) {
                        return;
                      }
                      // Mientras se arrastra, al fotograma clave (rápido); un clic o al soltar, al cuadro exacto.
                      const bool dragging = m_timeline != nullptr && m_timeline->dragging();
                      m_player.seekTo(value, !dragging);
                      if (m_positionLabel != nullptr) {
                        m_positionLabel->setText(formatTime(value));
                      }
                      wake();
                    },
                .onDragEnd =
                    [this] {
                      if (m_timeline != nullptr) {
                        m_player.seekTo(m_timeline->value(), true);
                      }
                    },
            }),
            ui::label({
                .out = &m_remainingLabel,
                .text = "−" + formatTime(state.duration - state.position),
                .fontSize = Style::fontSizeCaption * scale,
                .minWidth = 52.0F * scale,
                .textAlign = TextAlign::End,
            })
        )
    );

    auto buttons = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs * scale, .fillWidth = true});
    buttons->addChild(ghost("player-track-prev", tr("apps.play.previous"), m_index > 0, [this] { step(-1); }));
    buttons->addChild(ghost("rewind-backward-10", tr("apps.play.back"), state.loaded, [this] {
      m_player.seekBy(-10.0);
    }));
    buttons->addChild(
        ui::button({
            .glyph = state.paused || state.ended ? "player-play" : "player-pause",
            .variant = ButtonVariant::Primary,
            .tooltip = state.paused ? tr("apps.play.play") : tr("apps.play.pause"),
            .onClick = [this] {
              m_player.togglePause();
              wake();
            },
        })
    );
    buttons->addChild(ghost("rewind-forward-10", tr("apps.play.forward"), state.loaded, [this] {
      m_player.seekBy(10.0);
    }));
    buttons->addChild(ghost("player-track-next", tr("apps.play.next"), m_index + 1 < m_files.size(), [this] {
      step(1);
    }));

    const char* volumeGlyph = state.muted || state.volume <= 0.0 ? "volume-off"
        : state.volume < 50.0                                    ? "volume-2"
                                                                 : "volume";
    buttons->addChild(ghost(volumeGlyph, state.muted ? tr("apps.play.unmute") : tr("apps.play.mute"), true, [this] {
      m_player.setMuted(!m_player.state().muted);
    }));
    buttons->addChild(
        ui::slider({
            .minValue = 0.0,
            .maxValue = 100.0,
            .value = state.muted ? 0.0 : state.volume,
            .width = 100.0F * scale,
            .onValueChanged = [this](double value) {
              m_player.setVolume(value);
              if (m_player.state().muted) {
                m_player.setMuted(false);
              }
            },
        })
    );
    buttons->addChild(ui::node({.flexGrow = 1.0F}));

    // Velocidad.
    std::vector<std::string> speeds;
    std::size_t speedIndex = 2;
    for (std::size_t i = 0; i < kSpeeds.size(); ++i) {
      speeds.push_back(std::format("{}×", kSpeeds[i]));
      if (std::abs(kSpeeds[i] - state.speed) < 0.01) {
        speedIndex = i;
      }
    }
    buttons->addChild(
        ui::select({
            .options = std::move(speeds),
            .selectedIndex = speedIndex,
            .width = 84.0F * scale,
            .onSelectionChanged = [this](std::size_t index, std::string_view) { m_player.setSpeed(kSpeeds[index]); },
        })
    );

    // Pistas de audio (solo si hay más de una) y subtítulos (si hay alguno).
    std::vector<Track> audio;
    std::vector<Track> subs;
    for (const auto& track : state.tracks) {
      if (track.type == "audio") {
        audio.push_back(track);
      } else if (track.type == "sub") {
        subs.push_back(track);
      }
    }
    if (audio.size() > 1) {
      std::vector<std::string> names;
      std::size_t selected = 0;
      for (std::size_t i = 0; i < audio.size(); ++i) {
        names.push_back(trackName(audio[i], i));
        selected = audio[i].selected ? i : selected;
      }
      buttons->addChild(
          ui::select({
              .options = std::move(names),
              .selectedIndex = selected,
              .width = 150.0F * scale,
              .onSelectionChanged = [this, audio](std::size_t index, std::string_view) {
                m_player.selectTrack("audio", audio[index].id);
              },
          })
      );
    }
    if (!subs.empty()) {
      std::vector<std::string> names{tr("apps.play.subtitles-off")};
      std::size_t selected = 0;
      for (std::size_t i = 0; i < subs.size(); ++i) {
        names.push_back(trackName(subs[i], i));
        selected = subs[i].selected ? i + 1 : selected;
      }
      buttons->addChild(
          ui::select({
              .options = std::move(names),
              .selectedIndex = selected,
              .width = 150.0F * scale,
              .onSelectionChanged = [this, subs](std::size_t index, std::string_view) {
                m_player.selectTrack("sub", index == 0 ? -1 : subs[index - 1].id);
              },
          })
      );
    }
    buttons->addChild(
        ghost(m_app.fullscreen() ? "arrows-minimize" : "arrows-maximize", tr("apps.play.fullscreen"), true, [this] {
          toggleFullscreen();
        })
    );
    panel->addChild(std::move(buttons));

    // Con el puntero encima los controles no se ocultan.
    if (Renderer* renderer = m_app.renderer(); renderer != nullptr) {
      panel->layout(*renderer);
    }
    auto area = ui::inputArea({
        .width = width,
        .height = panel->height(),
        .onEnter = [this](const InputArea::PointerData&) { m_overControls = true; },
        .onLeave = [this] { m_overControls = false; },
    });
    area->addChild(std::move(panel));
    return area;
  }

  std::unique_ptr<Node> PlayerWindow::buildAudioCover(float width, float height, float scale) {
    const auto& state = m_player.state();
    auto cover = ui::column({
        .align = FlexAlign::Center,
        .justify = FlexJustify::Center,
        .gap = Style::spaceSm * scale,
        .width = width,
        .height = height - 120.0F * scale, // deja sitio a los controles
    });
    if (m_files.empty()) {
      cover->addChild(
          ui::glyph(
              {.glyph = "movie", .glyphSize = 72.0F * scale, .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}
          )
      );
      cover->addChild(
          ui::label({
              .text = tr("apps.play.empty.title"),
              .fontSize = Style::fontSizeTitle * scale,
              .fontWeight = FontWeight::SemiBold,
          })
      );
      cover->addChild(
          ui::label({
              .text = tr("apps.play.empty.hint"),
              .fontSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          })
      );
      cover->addChild(
          ui::button({
              .text = tr("apps.play.open"),
              .glyph = "folder-open",
              .variant = ButtonVariant::Primary,
              .onClick = [this] { openDialog(); },
          })
      );
      return cover;
    }
    cover->addChild(
        ui::glyph({.glyph = "music", .glyphSize = 96.0F * scale, .color = colorSpecFromRole(ColorRole::Primary)})
    );
    cover->addChild(
        ui::label({
            .text = !state.title.empty() ? state.title : m_files[m_index].filename().string(),
            .fontSize = Style::fontSizeHeader * scale,
            .fontWeight = FontWeight::SemiBold,
            .maxWidth = width * 0.8F,
            .maxLines = 2,
            .textAlign = TextAlign::Center,
        })
    );
    for (const auto& line : {state.artist, state.album}) {
      if (!line.empty()) {
        cover->addChild(
            ui::label({
                .text = line,
                .fontSize = Style::fontSizeBody * scale,
                .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                .maxWidth = width * 0.8F,
            })
        );
      }
    }
    return cover;
  }

  bool PlayerWindow::onKey(const KeyboardEvent& event) {
    if (!event.pressed || event.preedit) {
      return false;
    }
    const bool ctrl = (event.modifiers & KeyMod::Ctrl) != 0;
    const bool shift = (event.modifiers & KeyMod::Shift) != 0;
    const auto& state = m_player.state();
    wake();
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
    case XKB_KEY_space:
    case XKB_KEY_k:
    case XKB_KEY_XF86AudioPlay:
      m_player.togglePause();
      return true;
    case XKB_KEY_Left:
      m_player.seekBy(shift ? -1.0 : -5.0);
      return true;
    case XKB_KEY_Right:
      m_player.seekBy(shift ? 1.0 : 5.0);
      return true;
    case XKB_KEY_j:
      m_player.seekBy(-10.0);
      return true;
    case XKB_KEY_l:
      m_player.seekBy(10.0);
      return true;
    case XKB_KEY_Home:
      m_player.seekTo(0.0, true);
      return true;
    case XKB_KEY_Up:
      m_player.setVolume(state.volume + 5.0);
      return true;
    case XKB_KEY_Down:
      m_player.setVolume(state.volume - 5.0);
      return true;
    case XKB_KEY_m:
      m_player.setMuted(!state.muted);
      return true;
    case XKB_KEY_bracketleft:
      m_player.setSpeed(state.speed - 0.25);
      return true;
    case XKB_KEY_bracketright:
      m_player.setSpeed(state.speed + 0.25);
      return true;
    case XKB_KEY_BackSpace:
      m_player.setSpeed(1.0);
      return true;
    case XKB_KEY_n:
    case XKB_KEY_Page_Down:
    case XKB_KEY_XF86AudioNext:
      step(1);
      return true;
    case XKB_KEY_p:
    case XKB_KEY_Page_Up:
    case XKB_KEY_XF86AudioPrev:
      step(-1);
      return true;
    case XKB_KEY_s:
      m_player.cycle("sub");
      return true;
    case XKB_KEY_a:
      m_player.cycle("audio");
      return true;
    case XKB_KEY_f:
    case XKB_KEY_F11:
      toggleFullscreen();
      return true;
    case XKB_KEY_Escape:
      if (m_app.fullscreen()) {
        toggleFullscreen();
      }
      return true;
    default:
      return false;
    }
  }

} // namespace okaru::player
