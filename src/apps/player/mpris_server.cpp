#include "apps/player/mpris_server.h"

#include "apps/player/mpv_player.h"
#include "core/log.h"
#include "dbus/session_bus.h"
#include "i18n/i18n.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace okaru::player {

  namespace {

    constexpr Logger kLog("mpris");
    const sdbus::ObjectPath kObjectPath{"/org/mpris/MediaPlayer2"};
    constexpr auto kRootInterface = "org.mpris.MediaPlayer2";
    constexpr auto kPlayerInterface = "org.mpris.MediaPlayer2.Player";
    constexpr std::string_view kBusName = "org.mpris.MediaPlayer2.okaru_play";
    constexpr double kMinRate = 0.25;
    constexpr double kMaxRate = 4.0;
    constexpr double kSeekThreshold = 1.0; // segundos de diferencia con lo esperado para avisar Seeked

    constexpr double kUs = 1'000'000.0;

    std::int64_t toUs(double seconds) { return static_cast<std::int64_t>(std::llround(seconds * kUs)); }

    std::string trackPath(std::size_t index) { return "/dev/okaru/Player/track/" + std::to_string(index); }

  } // namespace

  MprisServer::MprisServer(SessionBus& bus, MpvPlayer& player, Hooks hooks)
      : m_bus(bus), m_player(player), m_hooks(std::move(hooks)) {
    auto& connection = m_bus.connection();
    m_object = sdbus::createObject(connection, kObjectPath);

    m_object
        ->addVTable(
            sdbus::registerMethod("Raise").implementedAs([] {}),
            sdbus::registerMethod("Quit").implementedAs([this] { m_hooks.quit(); }),
            sdbus::registerProperty("CanQuit").withGetter([] { return true; }),
            sdbus::registerProperty("CanRaise").withGetter([] { return false; }),
            sdbus::registerProperty("HasTrackList").withGetter([] { return false; }),
            sdbus::registerProperty("Identity").withGetter([] { return i18n::tr("apps.play.title"); }),
            sdbus::registerProperty("DesktopEntry").withGetter([] { return std::string("dev.okaru.Player"); }),
            sdbus::registerProperty("SupportedUriSchemes").withGetter([] {
              return std::vector<std::string>{"file", "http", "https"};
            }),
            sdbus::registerProperty("SupportedMimeTypes").withGetter([] {
              return std::vector<std::string>{"video/*", "audio/*"};
            })
        )
        .forInterface(kRootInterface);

    m_object
        ->addVTable(
            sdbus::registerMethod("Next").implementedAs([this] { m_hooks.step(1); }),
            sdbus::registerMethod("Previous").implementedAs([this] { m_hooks.step(-1); }),
            sdbus::registerMethod("Pause").implementedAs([this] { m_player.setPaused(true); }),
            sdbus::registerMethod("Play").implementedAs([this] { m_player.setPaused(false); }),
            sdbus::registerMethod("PlayPause").implementedAs([this] { m_player.togglePause(); }),
            sdbus::registerMethod("Stop").implementedAs([this] {
              m_player.setPaused(true);
              m_player.seekTo(0.0, true);
            }),
            sdbus::registerMethod("Seek").withInputParamNames("Offset").implementedAs([this](std::int64_t offset) {
              m_player.seekBy(static_cast<double>(offset) / kUs);
            }),
            sdbus::registerMethod("SetPosition")
                .withInputParamNames("TrackId", "Position")
                .implementedAs([this](const sdbus::ObjectPath& track, std::int64_t position) {
                  // La especificación pide ignorar posiciones fuera del archivo o de otra pista.
                  const auto& state = m_player.state();
                  if (track == trackPath(m_hooks.index()) && position >= 0 && position <= toUs(state.duration)) {
                    m_player.seekTo(static_cast<double>(position) / kUs, true);
                  }
                }),
            sdbus::registerMethod("OpenUri").withInputParamNames("Uri").implementedAs([this](const std::string& uri) {
              m_hooks.open(uri);
            }),
            sdbus::registerProperty("PlaybackStatus").withGetter([this] { return snapshot().status; }),
            sdbus::registerProperty("Rate").withGetter(
                                               [this] { return m_player.state().speed; }
            ).withSetter([this](const double& rate) {
              if (rate > 0.0) {
                m_player.setSpeed(std::clamp(rate, kMinRate, kMaxRate));
              }
            }),
            sdbus::registerProperty("MinimumRate").withGetter([] { return kMinRate; }),
            sdbus::registerProperty("MaximumRate").withGetter([] { return kMaxRate; }),
            sdbus::registerProperty("Volume").withGetter(
                                                 [this] { return m_player.state().volume / 100.0; }
            ).withSetter([this](const double& volume) { m_player.setVolume(std::clamp(volume, 0.0, 1.0) * 100.0); }),
            sdbus::registerProperty("Position").withGetter([this] { return positionUs(); }),
            sdbus::registerProperty("Metadata").withGetter([this] {
              std::map<std::string, sdbus::Variant> metadata;
              const auto& state = m_player.state();
              metadata["mpris:trackid"] = sdbus::Variant(sdbus::ObjectPath{trackPath(m_hooks.index())});
              if (!state.loaded) {
                return metadata;
              }
              if (state.duration > 0.0) {
                metadata["mpris:length"] = sdbus::Variant(toUs(state.duration));
              }
              metadata["xesam:title"] = sdbus::Variant(state.title);
              if (!state.artist.empty()) {
                metadata["xesam:artist"] = sdbus::Variant(std::vector<std::string>{state.artist});
              }
              if (!state.album.empty()) {
                metadata["xesam:album"] = sdbus::Variant(state.album);
              }
              const std::string path = m_hooks.currentPath();
              metadata["xesam:url"] = sdbus::Variant(path.contains("://") ? path : "file://" + path);
              return metadata;
            }),
            sdbus::registerProperty("CanGoNext").withGetter([this] { return m_hooks.canStep(1); }),
            sdbus::registerProperty("CanGoPrevious").withGetter([this] { return m_hooks.canStep(-1); }),
            sdbus::registerProperty("CanPlay").withGetter([this] { return m_player.state().loaded; }),
            sdbus::registerProperty("CanPause").withGetter([this] { return m_player.state().loaded; }),
            sdbus::registerProperty("CanSeek").withGetter([this] { return m_player.state().loaded; }),
            sdbus::registerProperty("CanControl").withGetter([] { return true; }),
            sdbus::registerSignal("Seeked").withParameters<std::int64_t>("Position")
        )
        .forInterface(kPlayerInterface);

    // Con otra ventana del reproductor abierta, el nombre corto está ocupado: se usa uno por proceso.
    try {
      connection.requestName(sdbus::ServiceName{std::string(kBusName)});
    } catch (const sdbus::Error&) {
      connection.requestName(sdbus::ServiceName{std::string(kBusName) + ".instance" + std::to_string(::getpid())});
    }
    m_last = snapshot();
    m_lastPositionAt = std::chrono::steady_clock::now();
  }

  MprisServer::~MprisServer() = default;

  std::int64_t MprisServer::positionUs() const { return toUs(m_player.state().position); }

  MprisServer::Snapshot MprisServer::snapshot() const {
    const auto& state = m_player.state();
    return {
        .status = !state.loaded ? "Stopped" : (state.paused || state.ended ? "Paused" : "Playing"),
        .trackId = trackPath(m_hooks.index()),
        .title = state.title,
        .duration = state.duration,
        .volume = state.volume,
        .rate = state.speed,
        .canPrevious = m_hooks.canStep(-1),
        .canNext = m_hooks.canStep(1),
        .loaded = state.loaded,
    };
  }

  void MprisServer::update() {
    const Snapshot now = snapshot();
    std::vector<sdbus::PropertyName> changed;
    if (now.status != m_last.status) {
      changed.emplace_back("PlaybackStatus");
    }
    if (now.trackId != m_last.trackId
        || now.title != m_last.title
        || now.duration != m_last.duration
        || now.loaded != m_last.loaded) {
      changed.emplace_back("Metadata");
    }
    if (now.volume != m_last.volume) {
      changed.emplace_back("Volume");
    }
    if (now.rate != m_last.rate) {
      changed.emplace_back("Rate");
    }
    if (now.canNext != m_last.canNext) {
      changed.emplace_back("CanGoNext");
    }
    if (now.canPrevious != m_last.canPrevious) {
      changed.emplace_back("CanGoPrevious");
    }
    if (now.loaded != m_last.loaded) {
      changed.insert(
          changed.end(),
          {sdbus::PropertyName{"CanPlay"}, sdbus::PropertyName{"CanPause"}, sdbus::PropertyName{"CanSeek"}}
      );
    }

    // Position no avisa por PropertiesChanged: los clientes la extrapolan y solo esperan Seeked en los saltos.
    const auto clock = std::chrono::steady_clock::now();
    const double position = m_player.state().position;
    const double elapsed = std::chrono::duration<double>(clock - m_lastPositionAt).count();
    const double expected = m_lastPosition + (m_last.status == "Playing" ? elapsed * m_last.rate : 0.0);
    const bool seeked = now.trackId == m_last.trackId && std::abs(position - expected) > kSeekThreshold;
    m_lastPosition = position;
    m_lastPositionAt = clock;
    m_last = now;

    try {
      if (!changed.empty()) {
        m_object->emitPropertiesChangedSignal(kPlayerInterface, changed);
      }
      if (seeked) {
        m_object->emitSignal("Seeked").onInterface(kPlayerInterface).withArguments(toUs(position));
      }
    } catch (const sdbus::Error& e) {
      kLog.debug("MPRIS signal failed: {}", e.what());
    }
  }

} // namespace okaru::player
