#pragma once

// OKARU: arrastrar y soltar archivos con wl_data_device. Recibe text/uri-list (rutas locales o URL tal cual) y, en
// okaru-files, también arrastra hacia afuera: un wl_data_source con la lista de URIs que acepta cualquier app.

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

struct wl_buffer;
struct wl_compositor;
struct wl_data_device;
struct wl_shm;
struct wl_data_device_manager;
struct wl_data_offer;
struct wl_data_source;
struct wl_surface;
struct wl_display;
struct wl_seat;

namespace okaru::apps {

  // Líneas de un text/uri-list: file:// → ruta local, otros esquemas sin tocar; ignora comentarios y vacías.
  [[nodiscard]] std::vector<std::string> parseUriList(std::string_view list);

  // Imagen que sigue al puntero mientras se arrastra: ARGB8888 premultiplicado; el puntero queda en (hotspotX,
  // hotspotY).
  struct DragIcon {
    int width = 0;
    int height = 0;
    int hotspotX = 0;
    int hotspotY = 0;
    std::vector<std::uint32_t> pixels;
  };

  class FileDrop {
  public:
    using DropFn = std::function<void(std::vector<std::string> paths)>;
    // Con la posición (coordenadas de la superficie) y si el arrastre salió de esta misma app.
    using DropAtFn = std::function<void(std::vector<std::string> paths, float x, float y, bool own)>;
    using MotionFn = std::function<void(float x, float y)>;

    FileDrop() = default;
    ~FileDrop();

    FileDrop(const FileDrop&) = delete;
    FileDrop& operator=(const FileDrop&) = delete;

    // Busca wl_data_device_manager en el registro; sin él no hay soltar (no es un error).
    void attach(wl_display* display, wl_seat* seat);
    void setOnDrop(DropFn callback) { m_onDrop = std::move(callback); }
    void setOnDropAt(DropAtFn callback) { m_onDropAt = std::move(callback); }
    // Mientras se arrastra encima (para resaltar destinos); (-1, -1) al salir.
    void setOnMotion(MotionFn callback) { m_onMotion = std::move(callback); }
    // Empieza a arrastrar `uris` (file://…) desde `origin`; `serial` es el del clic que lo inició.
    bool startDrag(wl_surface* origin, unsigned serial, std::vector<std::string> uris, const DragIcon& icon = {});
    [[nodiscard]] bool dragging() const noexcept { return m_source != nullptr; }
    void onSourceSend(const char* mime, int fd);
    void onSourceDone();

    // Llamados desde los listeners de Wayland.
    void onBind(wl_data_device_manager* manager, unsigned version);
    void onBindIconGlobals(wl_compositor* compositor, wl_shm* shm);
    void onOffer(wl_data_offer* offer);
    void onOfferMime(wl_data_offer* offer, const char* mime);
    void onEnter(unsigned serial, wl_data_offer* offer, float x, float y);
    void onMotionAt(float x, float y);
    void onLeave();
    void onDrop();
    void onSelection(wl_data_offer* offer);

  private:
    void dropOffer(wl_data_offer* offer);
    [[nodiscard]] wl_surface* createIconSurface(const DragIcon& icon);
    void destroyIconSurface();

    wl_display* m_display = nullptr;
    wl_data_device_manager* m_manager = nullptr;
    wl_data_device* m_device = nullptr;
    unsigned m_version = 0;
    wl_data_offer* m_dragOffer = nullptr;
    std::set<wl_data_offer*> m_uriOffers; // ofertas que traen text/uri-list
    DropFn m_onDrop;
    DropAtFn m_onDropAt;
    MotionFn m_onMotion;
    float m_lastX = -1.0F;
    float m_lastY = -1.0F;
    wl_data_source* m_source = nullptr;
    std::vector<std::string> m_dragUris;
    wl_compositor* m_compositor = nullptr;
    wl_shm* m_shm = nullptr;
    wl_surface* m_iconSurface = nullptr;
    wl_buffer* m_iconBuffer = nullptr;
  };

} // namespace okaru::apps
