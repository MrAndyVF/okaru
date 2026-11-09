#pragma once

// OKARU: interfaz de okaru-view, al estilo del visor de GNOME (Loupe): una imagen a la vez, recorrer la carpeta,
// zoom (rueda, teclado, doble clic), arrastrar para mover, girar, animaciones, propiedades y acciones (copiar,
// fondo de pantalla, papelera, abrir la carpeta, pantalla completa). Decodifica en otro hilo.

#include "apps/viewer/viewer_image.h"
#include "core/timer_manager.h"
#include "render/core/texture_handle.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class Image;
class InputArea;
class Node;
class Renderer;
struct KeyboardEvent;

namespace okaru::apps {
  class OkaruApp;
  class PortalFileChooser;
} // namespace okaru::apps

namespace okaru::viewer {

  class ViewerWindow {
  public:
    ViewerWindow(apps::OkaruApp& app, const std::vector<std::filesystem::path>& files);
    ~ViewerWindow();

  private:
    [[nodiscard]] std::unique_ptr<Node> build(float width, float height, float scale);
    [[nodiscard]] std::unique_ptr<Node> buildHeader(float width, float scale);
    [[nodiscard]] std::unique_ptr<Node> buildCanvas(float width, float height, float scale);
    [[nodiscard]] std::unique_ptr<Node> buildProperties(float width, float height, float scale);
    [[nodiscard]] bool onKey(const KeyboardEvent& event);

    void openFiles(const std::vector<std::filesystem::path>& files);
    void show(std::size_t index);
    // `refresh` = misma imagen con más detalle (SVG más grande o JPEG completo): conserva la vista.
    void decode(bool refresh, int svgSize = 0);
    void prefetchNeighbors();
    // Lado en píxeles físicos que ocupa el lienzo: hasta ahí basta decodificar para ver la imagen ajustada.
    [[nodiscard]] int screenFitSize() const;
    void uploadPending(Renderer& renderer);
    void unloadTextures();
    void startAnimation();

    // Geometría: zoom 1 = un píxel de la imagen original por píxel lógico.
    [[nodiscard]] float fitZoom() const;
    [[nodiscard]] float currentZoom() const;
    void layoutImage();
    void zoomBy(float factor, float anchorX, float anchorY);
    void setZoom(float zoom, float anchorX, float anchorY);
    void fit();
    void clampPan();
    void maybeLoadMoreDetail();

    void step(int delta);
    void rotate(int quarterTurns);
    void toggleFullscreen();
    void copyImage();
    void setWallpaper();
    void moveToTrash();
    void showInFolder();
    void openDialog();

    apps::OkaruApp& m_app;
    std::unique_ptr<apps::PortalFileChooser> m_chooser;
    std::vector<std::filesystem::path> m_files;
    std::size_t m_index = 0;

    // Resultado de la decodificación, compartido con el hilo que la hace.
    std::shared_ptr<std::atomic<unsigned>> m_generation = std::make_shared<std::atomic<unsigned>>(0);
    std::unique_ptr<DecodedImage> m_decoded; // cuadros aún sin subir a la GPU
    // La imagen completa cuando en pantalla hay una copia reducida: al ampliar se sube sin decodificar de nuevo.
    std::unique_ptr<DecodedImage> m_full;
    bool m_loading = false;
    std::string m_error;

    std::vector<TextureHandle> m_textures;
    std::vector<std::uint32_t> m_durations;
    std::size_t m_frame = 0;
    Timer m_frameTimer;
    Timer m_detailTimer; // espera a que termine el gesto de zoom antes de pedir más detalle
    int m_textureWidth = 0;
    int m_textureHeight = 0;
    int m_sourceWidth = 0;
    int m_sourceHeight = 0;
    bool m_vector = false;
    bool m_reduced = false; // la textura tiene menos detalle que el archivo (JPEG reducido para la pantalla)

    // Vecinas decodificadas de antemano: pasar con ←/→ no espera al disco ni al decodificador.
    struct Prefetched {
      std::filesystem::path path;
      std::shared_ptr<DecodedImage> image;
      std::vector<MetadataField> camera;
    };
    std::vector<Prefetched> m_prefetched;
    std::vector<std::filesystem::path> m_prefetching;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
    std::vector<MetadataField> m_camera;

    bool m_fitMode = true;
    float m_zoom = 1.0F;
    float m_panX = 0.0F; // desplazamiento del centro de la imagen respecto al centro del lienzo
    float m_panY = 0.0F;
    int m_quarterTurns = 0;
    bool m_showProperties = false;
    std::string m_status; // aviso breve bajo la cabecera ("Copiada", "Movida a la papelera"…)
    Timer m_statusTimer;

    bool m_dragging = false;
    float m_dragStartX = 0.0F;
    float m_dragStartY = 0.0F;
    float m_dragPanX = 0.0F;
    float m_dragPanY = 0.0F;
    std::uint32_t m_lastClickTime = 0;

    Node* m_canvas = nullptr;
    Image* m_image = nullptr;
    InputArea* m_canvasInput = nullptr;
    float m_canvasWidth = 0.0F;
    float m_canvasHeight = 0.0F;
  };

} // namespace okaru::viewer
