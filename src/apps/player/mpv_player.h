#pragma once

// OKARU: motor de okaru-play sobre libmpv. mpv decodifica (todos los formatos de ffmpeg, decodificación por GPU),
// sincroniza audio y video y saca el audio por PipeWire; aquí solo se le dan órdenes, se leen sus propiedades y se le
// pide que dibuje cada cuadro en un framebuffer de OKARU. Todo se usa desde el hilo principal.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct mpv_handle;
struct mpv_render_context;
struct mpv_node;
struct wl_display;

namespace okaru::player {

  struct Track {
    std::int64_t id = 0;
    std::string type; // "video", "audio" o "sub"
    std::string title;
    std::string lang;
    bool selected = false;
    bool albumArt = false; // portada de un audio, no un video

    bool operator==(const Track&) const = default;
  };

  struct PlayerState {
    double position = 0.0;
    double duration = 0.0;
    double volume = 100.0;
    double speed = 1.0;
    bool paused = true;
    bool muted = false;
    bool ended = false; // llegó al final (keep-open: queda en el último cuadro)
    bool loaded = false;
    std::string title;
    std::string artist;
    std::string album;
    std::vector<Track> tracks;

    [[nodiscard]] bool hasVideo() const;
  };

  // Cambio en el estado: `structural` = cambió algo más que la posición (pistas, pausa, archivo…).
  using ChangeFn = std::function<void(bool structural)>;

  class MpvPlayer {
  public:
    explicit MpvPlayer(const std::filesystem::path& watchLaterDir);
    ~MpvPlayer();

    MpvPlayer(const MpvPlayer&) = delete;
    MpvPlayer& operator=(const MpvPlayer&) = delete;

    [[nodiscard]] bool valid() const noexcept { return m_mpv != nullptr; }
    [[nodiscard]] const PlayerState& state() const noexcept { return m_state; }

    void setOnChange(ChangeFn callback) { m_onChange = std::move(callback); }
    void setOnFrame(std::function<void()> callback) { m_onFrame = std::move(callback); }

    void load(const std::string& path);
    void setPaused(bool paused);
    void togglePause() { setPaused(!m_state.paused); }
    // `exact` = al cuadro (al soltar la barra); si no, al fotograma clave más cercano (rápido, mientras se arrastra).
    void seekTo(double seconds, bool exact);
    void seekBy(double seconds);
    void setVolume(double volume);
    void setMuted(bool muted);
    void setSpeed(double speed);
    // id < 0 = desactivar (solo subtítulos).
    void selectTrack(const std::string& type, std::int64_t id);
    // Pasa a la siguiente pista ("sub", "audio").
    void cycle(const std::string& property) { command({"cycle", property}); }
    // Guarda la posición para retomar el archivo después (watch-later de mpv).
    void rememberPosition();

    // Render: llamar con el contexto GL de OKARU activo.
    bool initRender(wl_display* display);
    // Dibuja en `fbo` si mpv tiene un cuadro nuevo; true si dibujó.
    bool renderIfNeeded(unsigned fbo, int width, int height, bool force);
    void freeRender();

  private:
    void drainEvents();
    void onProperty(const char* name, int format, void* data);
    void command(const std::vector<std::string>& args);

    mpv_handle* m_mpv = nullptr;
    mpv_render_context* m_render = nullptr;
    PlayerState m_state;
    ChangeFn m_onChange;
    std::function<void()> m_onFrame;
    // mpv avisa desde sus hilos; se junta en una sola llamada al hilo principal.
    std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>>(true);
    std::atomic<bool> m_eventsPending{false};
    std::atomic<bool> m_framePending{false};
  };

  // Archivos de audio y video de la carpeta, en orden natural.
  [[nodiscard]] bool isMediaFile(const std::filesystem::path& path);
  [[nodiscard]] std::vector<std::filesystem::path> siblingMedia(const std::filesystem::path& file);
  // "1:05", "1:02:03".
  [[nodiscard]] std::string formatTime(double seconds);

} // namespace okaru::player
