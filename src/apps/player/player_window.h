#pragma once

// OKARU: interfaz de okaru-play. Video a pantalla completa del lienzo con controles que se ocultan solos al reproducir,
// barra de tiempo, volumen, velocidad, pistas de audio y subtítulos, lista de la carpeta, pantalla completa y portada
// con datos para el audio. mpv dibuja cada cuadro en un framebuffer de OKARU que la escena muestra como imagen.

#include "apps/player/mpv_player.h"
#include "core/timer_manager.h"
#include "render/backend/gles_framebuffer.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

class Image;
class Label;
class Node;
class Renderer;
class Slider;
struct KeyboardEvent;

namespace okaru::apps {
  class OkaruApp;
  class PortalFileChooser;
} // namespace okaru::apps

namespace okaru::player {

  class PlayerWindow {
  public:
    PlayerWindow(apps::OkaruApp& app, const std::vector<std::filesystem::path>& files);
    ~PlayerWindow();

  private:
    [[nodiscard]] std::unique_ptr<Node> build(float width, float height, float scale);
    [[nodiscard]] std::unique_ptr<Node> buildHeader(float width, float scale);
    [[nodiscard]] std::unique_ptr<Node> buildControls(float width, float scale);
    [[nodiscard]] std::unique_ptr<Node> buildAudioCover(float width, float height, float scale);
    [[nodiscard]] bool onKey(const KeyboardEvent& event);
    void beforeRender(Renderer& renderer);
    void onPlayerChange(bool structural);

    void openFiles(const std::vector<std::filesystem::path>& files);
    void play(std::size_t index);
    void step(int delta);
    void toggleFullscreen();
    void openDialog();
    // Muestra los controles y reinicia la cuenta para ocultarlos.
    void wake();
    void hideControls();
    void updateTimeline();

    apps::OkaruApp& m_app;
    MpvPlayer m_player;
    std::unique_ptr<apps::PortalFileChooser> m_chooser;
    std::vector<std::filesystem::path> m_files;
    std::size_t m_index = 0;

    GlesFramebuffer m_frame; // mpv dibuja aquí, al tamaño del lienzo en píxeles físicos
    bool m_renderReady = false;
    // mpv necesita el contexto de render antes de abrir un video (si no, desactiva la pista de video):
    // lo que se pide antes del primer cuadro espera aquí.
    std::optional<std::size_t> m_pendingPlay;
    bool m_forceRender = false;
    float m_canvasWidth = 0.0F;
    float m_canvasHeight = 0.0F;

    bool m_controlsVisible = true;
    bool m_overControls = false;
    Timer m_hideTimer;
    Timer m_clickTimer; // un clic pausa; si llega el segundo a tiempo, es doble clic (pantalla completa)
    std::uint32_t m_lastClickTime = 0;
    // Al reconstruir, el área nueva recibe un movimiento sin que el puntero se mueva: solo cuenta uno real.
    float m_pointerX = -1.0F;
    bool m_syncingTimeline = false; // Slider::setValue avisa como si lo moviera el usuario
    float m_pointerY = -1.0F;

    Image* m_video = nullptr;
    Slider* m_timeline = nullptr;
    Label* m_positionLabel = nullptr;
    Label* m_remainingLabel = nullptr;
  };

} // namespace okaru::player
