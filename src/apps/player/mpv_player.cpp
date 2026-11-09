#include "apps/player/mpv_player.h"

#include "core/deferred_call.h"
#include "core/log.h"

#include <EGL/egl.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <glib.h>
#include <mpv/client.h>
#include <mpv/render_gl.h>
#include <strings.h>

namespace okaru::player {

  namespace {

    namespace fs = std::filesystem;
    constexpr Logger kLog("player");

    constexpr std::array<std::string_view, 38> kMediaExtensions{
        // video
        "mp4",
        "m4v",
        "mkv",
        "webm",
        "avi",
        "mov",
        "wmv",
        "flv",
        "mpg",
        "mpeg",
        "m2ts",
        "mts",
        "ts",
        "ogv",
        "3gp",
        "vob",
        "divx",
        "f4v",
        "rmvb",
        "asf",
        // audio
        "mp3",
        "flac",
        "ogg",
        "oga",
        "opus",
        "m4a",
        "aac",
        "wav",
        "wma",
        "aiff",
        "aif",
        "ape",
        "wv",
        "mka",
        "ac3",
        "dts",
        "mpc",
        "spx",
    };

    std::string lowerExtension(const fs::path& path) {
      std::string ext = path.extension().string();
      if (!ext.empty()) {
        ext.erase(0, 1);
      }
      std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return ext;
    }

    const mpv_node* mapValue(const mpv_node& map, const char* key) {
      if (map.format != MPV_FORMAT_NODE_MAP) {
        return nullptr;
      }
      for (int i = 0; i < map.u.list->num; ++i) {
        if (strcasecmp(map.u.list->keys[i], key) == 0) {
          return &map.u.list->values[i];
        }
      }
      return nullptr;
    }

    std::string nodeString(const mpv_node* node) {
      return node != nullptr && node->format == MPV_FORMAT_STRING ? std::string(node->u.string) : std::string();
    }

    void* getProcAddress(void* /*ctx*/, const char* name) { return reinterpret_cast<void*>(eglGetProcAddress(name)); }

  } // namespace

  bool PlayerState::hasVideo() const {
    return std::ranges::any_of(tracks, [](const Track& t) { return t.type == "video" && t.selected; });
  }

  MpvPlayer::MpvPlayer(const fs::path& watchLaterDir) {
    m_mpv = mpv_create();
    if (m_mpv == nullptr) {
      kLog.error("mpv_create failed");
      return;
    }
    std::error_code ec;
    fs::create_directories(watchLaterDir, ec);
    const std::array<std::pair<const char*, std::string>, 11> options{{
        {"vo", "libmpv"},
        {"hwdec", "auto-safe"},
        {"keep-open", "yes"}, // al terminar se queda en el último cuadro; la app pasa al siguiente
        {"idle", "yes"},
        {"terminal", "no"},
        {"input-default-bindings", "no"},
        {"osc", "no"},
        {"audio-display", "embedded-first"}, // portada del audio como imagen
        {"save-position-on-quit", "yes"},
        {"watch-later-directory", watchLaterDir.string()},
        {"volume-max", "100"},
    }};
    for (const auto& [name, value] : options) {
      mpv_set_option_string(m_mpv, name, value.c_str());
    }
    if (mpv_initialize(m_mpv) < 0) {
      kLog.error("mpv_initialize failed");
      mpv_terminate_destroy(m_mpv);
      m_mpv = nullptr;
      return;
    }
    const std::array<std::pair<const char*, mpv_format>, 13> properties{{
        {"time-pos", MPV_FORMAT_DOUBLE},
        {"duration", MPV_FORMAT_DOUBLE},
        {"pause", MPV_FORMAT_FLAG},
        {"volume", MPV_FORMAT_DOUBLE},
        {"mute", MPV_FORMAT_FLAG},
        {"speed", MPV_FORMAT_DOUBLE},
        {"eof-reached", MPV_FORMAT_FLAG},
        {"media-title", MPV_FORMAT_STRING},
        {"metadata", MPV_FORMAT_NODE},
        {"track-list", MPV_FORMAT_NODE},
        {"idle-active", MPV_FORMAT_FLAG},
        {"dwidth", MPV_FORMAT_INT64},
        {"dheight", MPV_FORMAT_INT64},
    }};
    for (const auto& [name, format] : properties) {
      mpv_observe_property(m_mpv, 0, name, format);
    }
    // Los avisos de mpv (archivo dañado, códec sin soporte…) van al registro de OKARU.
    mpv_request_log_messages(m_mpv, "warn");
    mpv_set_wakeup_callback(
        m_mpv,
        [](void* ctx) {
          auto* self = static_cast<MpvPlayer*>(ctx);
          if (self->m_eventsPending.exchange(true)) {
            return;
          }
          DeferredCall::callLater([self, alive = self->m_alive] {
            if (*alive) {
              self->m_eventsPending = false;
              self->drainEvents();
            }
          });
        },
        this
    );
  }

  MpvPlayer::~MpvPlayer() {
    *m_alive = false;
    if (m_mpv != nullptr) {
      mpv_set_wakeup_callback(m_mpv, nullptr, nullptr);
      rememberPosition();
      // freeRender() ya se llamó con el contexto GL activo (OkaruApp::setOnClose); si no, se libera igual.
      if (m_render != nullptr) {
        mpv_render_context_free(m_render);
        m_render = nullptr;
      }
      mpv_terminate_destroy(m_mpv);
    }
  }

  void MpvPlayer::command(const std::vector<std::string>& args) {
    if (m_mpv == nullptr) {
      return;
    }
    std::vector<const char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& arg : args) {
      argv.push_back(arg.c_str());
    }
    argv.push_back(nullptr);
    mpv_command_async(m_mpv, 0, argv.data());
  }

  void MpvPlayer::load(const std::string& path) {
    rememberPosition();
    m_state.loaded = false;
    m_state.ended = false;
    m_state.position = 0.0;
    m_state.duration = 0.0;
    command({"loadfile", path, "replace"});
    setPaused(false);
  }

  void MpvPlayer::setPaused(bool paused) {
    if (m_mpv == nullptr) {
      return;
    }
    // Al final de un archivo, "reproducir" vuelve a empezar.
    if (!paused && m_state.ended) {
      seekTo(0.0, true);
    }
    int flag = paused ? 1 : 0;
    mpv_set_property_async(m_mpv, 0, "pause", MPV_FORMAT_FLAG, &flag);
  }

  void MpvPlayer::seekTo(double seconds, bool exact) {
    command({"seek", std::format("{:.3f}", std::max(0.0, seconds)), exact ? "absolute+exact" : "absolute+keyframes"});
  }

  void MpvPlayer::seekBy(double seconds) { command({"seek", std::format("{:.3f}", seconds), "relative+exact"}); }

  void MpvPlayer::setVolume(double volume) {
    if (m_mpv != nullptr) {
      double value = std::clamp(volume, 0.0, 100.0);
      mpv_set_property_async(m_mpv, 0, "volume", MPV_FORMAT_DOUBLE, &value);
    }
  }

  void MpvPlayer::setMuted(bool muted) {
    if (m_mpv != nullptr) {
      int flag = muted ? 1 : 0;
      mpv_set_property_async(m_mpv, 0, "mute", MPV_FORMAT_FLAG, &flag);
    }
  }

  void MpvPlayer::setSpeed(double speed) {
    if (m_mpv != nullptr) {
      double value = std::clamp(speed, 0.25, 4.0);
      mpv_set_property_async(m_mpv, 0, "speed", MPV_FORMAT_DOUBLE, &value);
    }
  }

  void MpvPlayer::selectTrack(const std::string& type, std::int64_t id) {
    const char* property = type == "video" ? "vid" : type == "audio" ? "aid" : "sid";
    if (m_mpv != nullptr) {
      const std::string value = id < 0 ? std::string("no") : std::to_string(id);
      mpv_set_property_string(m_mpv, property, value.c_str());
    }
  }

  void MpvPlayer::rememberPosition() {
    // Solo si hay un archivo a medias; sin archivo o al final, mpv no tiene nada útil que guardar.
    if (m_mpv != nullptr && m_state.loaded && !m_state.ended) {
      const char* args[] = {"write-watch-later-config", nullptr};
      mpv_command(m_mpv, args);
    }
  }

  bool MpvPlayer::initRender(wl_display* display) {
    if (m_mpv == nullptr || m_render != nullptr) {
      return m_render != nullptr;
    }
    mpv_opengl_init_params glInit{.get_proc_address = getProcAddress, .get_proc_address_ctx = nullptr};
    std::array<mpv_render_param, 4> params{{
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL)},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &glInit},
        {MPV_RENDER_PARAM_WL_DISPLAY, display}, // decodificación por GPU (VA-API) sin copias
        {MPV_RENDER_PARAM_INVALID, nullptr},
    }};
    if (mpv_render_context_create(&m_render, m_mpv, params.data()) < 0) {
      kLog.error("mpv_render_context_create failed");
      m_render = nullptr;
      return false;
    }
    mpv_render_context_set_update_callback(
        m_render,
        [](void* ctx) {
          auto* self = static_cast<MpvPlayer*>(ctx);
          if (self->m_framePending.exchange(true)) {
            return;
          }
          DeferredCall::callLater([self, alive = self->m_alive] {
            if (*alive && self->m_onFrame) {
              self->m_onFrame();
            }
          });
        },
        this
    );
    return true;
  }

  bool MpvPlayer::renderIfNeeded(unsigned fbo, int width, int height, bool force) {
    if (m_render == nullptr) {
      return false;
    }
    m_framePending = false;
    const std::uint64_t flags = mpv_render_context_update(m_render);
    if ((flags & MPV_RENDER_UPDATE_FRAME) == 0 && !force) {
      return false;
    }
    mpv_opengl_fbo target{.fbo = static_cast<int>(fbo), .w = width, .h = height, .internal_format = 0};
    // Sin invertir: la textura del framebuffer ya queda con la fila de arriba donde el Image la espera.
    int flip = 0;
    std::array<mpv_render_param, 3> params{{
        {MPV_RENDER_PARAM_OPENGL_FBO, &target},
        {MPV_RENDER_PARAM_FLIP_Y, &flip},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    }};
    mpv_render_context_render(m_render, params.data());
    return true;
  }

  void MpvPlayer::freeRender() {
    if (m_render != nullptr) {
      mpv_render_context_free(m_render);
      m_render = nullptr;
    }
  }

  void MpvPlayer::drainEvents() {
    bool structural = false;
    bool changed = false;
    while (m_mpv != nullptr) {
      mpv_event* event = mpv_wait_event(m_mpv, 0);
      if (event->event_id == MPV_EVENT_NONE) {
        break;
      }
      switch (event->event_id) {
      case MPV_EVENT_PROPERTY_CHANGE: {
        const auto* prop = static_cast<mpv_event_property*>(event->data);
        const std::string_view name = prop->name;
        onProperty(prop->name, prop->format, prop->data);
        changed = true;
        structural = structural || name != "time-pos";
        break;
      }
      case MPV_EVENT_LOG_MESSAGE: {
        const auto* message = static_cast<mpv_event_log_message*>(event->data);
        std::string_view text = message->text;
        if (text.ends_with('\n')) {
          text.remove_suffix(1);
        }
        kLog.warn("mpv/{}: {}", message->prefix, text);
        break;
      }
      case MPV_EVENT_FILE_LOADED:
        m_state.loaded = true;
        m_state.ended = false;
        changed = structural = true;
        break;
      case MPV_EVENT_END_FILE: {
        const auto* end = static_cast<mpv_event_end_file*>(event->data);
        if (end->reason == MPV_END_FILE_REASON_ERROR) {
          kLog.warn("mpv: {}", mpv_error_string(end->error));
          m_state.loaded = false;
          changed = structural = true;
        }
        break;
      }
      default:
        break;
      }
    }
    if (changed && m_onChange) {
      m_onChange(structural);
    }
  }

  void MpvPlayer::onProperty(const char* name, int format, void* data) {
    const std::string_view key = name;
    const bool present = format != MPV_FORMAT_NONE && data != nullptr;
    if (key == "time-pos") {
      m_state.position = present ? *static_cast<double*>(data) : 0.0;
    } else if (key == "duration") {
      m_state.duration = present ? *static_cast<double*>(data) : 0.0;
    } else if (key == "volume") {
      m_state.volume = present ? *static_cast<double*>(data) : m_state.volume;
    } else if (key == "speed") {
      m_state.speed = present ? *static_cast<double*>(data) : 1.0;
    } else if (key == "pause") {
      m_state.paused = present && *static_cast<int*>(data) != 0;
    } else if (key == "mute") {
      m_state.muted = present && *static_cast<int*>(data) != 0;
    } else if (key == "eof-reached") {
      m_state.ended = present && *static_cast<int*>(data) != 0;
    } else if (key == "media-title") {
      m_state.title = present ? *static_cast<char**>(data) : "";
    } else if (key == "metadata") {
      m_state.artist.clear();
      m_state.album.clear();
      if (present) {
        const auto& map = *static_cast<mpv_node*>(data);
        m_state.artist = nodeString(mapValue(map, "artist"));
        m_state.album = nodeString(mapValue(map, "album"));
      }
    } else if (key == "track-list") {
      m_state.tracks.clear();
      if (present && static_cast<mpv_node*>(data)->format == MPV_FORMAT_NODE_ARRAY) {
        const auto* list = static_cast<mpv_node*>(data)->u.list;
        for (int i = 0; i < list->num; ++i) {
          const mpv_node& entry = list->values[i];
          Track track;
          if (const auto* id = mapValue(entry, "id"); id != nullptr && id->format == MPV_FORMAT_INT64) {
            track.id = id->u.int64;
          }
          track.type = nodeString(mapValue(entry, "type"));
          track.title = nodeString(mapValue(entry, "title"));
          track.lang = nodeString(mapValue(entry, "lang"));
          const auto* selected = mapValue(entry, "selected");
          track.selected = selected != nullptr && selected->format == MPV_FORMAT_FLAG && selected->u.flag != 0;
          const auto* art = mapValue(entry, "albumart");
          track.albumArt = art != nullptr && art->format == MPV_FORMAT_FLAG && art->u.flag != 0;
          m_state.tracks.push_back(std::move(track));
        }
      }
    }
  }

  bool isMediaFile(const fs::path& path) { return std::ranges::contains(kMediaExtensions, lowerExtension(path)); }

  std::vector<fs::path> siblingMedia(const fs::path& file) {
    std::vector<std::pair<std::string, fs::path>> keyed;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(file.parent_path(), ec)) {
      if (entry.is_regular_file(ec) && isMediaFile(entry.path())) {
        gchar* key = g_utf8_collate_key_for_filename(entry.path().filename().c_str(), -1);
        keyed.emplace_back(key, entry.path());
        g_free(key);
      }
    }
    std::ranges::sort(keyed, {}, &std::pair<std::string, fs::path>::first);
    std::vector<fs::path> out;
    out.reserve(keyed.size());
    for (auto& [key, path] : keyed) {
      out.push_back(std::move(path));
    }
    return out;
  }

  std::string formatTime(double seconds) {
    const auto total = static_cast<long>(std::max(0.0, std::floor(seconds)));
    const long hours = total / 3600;
    const long minutes = (total / 60) % 60;
    const long secs = total % 60;
    return hours > 0 ? std::format("{}:{:02}:{:02}", hours, minutes, secs) : std::format("{}:{:02}", minutes, secs);
  }

} // namespace okaru::player
