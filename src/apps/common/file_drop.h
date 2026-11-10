#pragma once

// OKARU: soltar archivos sobre una app (desde Nautilus u otro explorador) con wl_data_device. Solo acepta
// text/uri-list y entrega las rutas locales (file://) o las URL tal cual.

#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

struct wl_data_device;
struct wl_data_device_manager;
struct wl_data_offer;
struct wl_display;
struct wl_seat;

namespace okaru::apps {

  // Líneas de un text/uri-list: file:// → ruta local, otros esquemas sin tocar; ignora comentarios y vacías.
  [[nodiscard]] std::vector<std::string> parseUriList(std::string_view list);

  class FileDrop {
  public:
    using DropFn = std::function<void(std::vector<std::string> paths)>;

    FileDrop() = default;
    ~FileDrop();

    FileDrop(const FileDrop&) = delete;
    FileDrop& operator=(const FileDrop&) = delete;

    // Busca wl_data_device_manager en el registro; sin él no hay soltar (no es un error).
    void attach(wl_display* display, wl_seat* seat);
    void setOnDrop(DropFn callback) { m_onDrop = std::move(callback); }

    // Llamados desde los listeners de Wayland.
    void onBind(wl_data_device_manager* manager, unsigned version);
    void onOffer(wl_data_offer* offer);
    void onOfferMime(wl_data_offer* offer, const char* mime);
    void onEnter(unsigned serial, wl_data_offer* offer);
    void onLeave();
    void onDrop();
    void onSelection(wl_data_offer* offer);

  private:
    void dropOffer(wl_data_offer* offer);

    wl_display* m_display = nullptr;
    wl_data_device_manager* m_manager = nullptr;
    wl_data_device* m_device = nullptr;
    unsigned m_version = 0;
    wl_data_offer* m_dragOffer = nullptr;
    std::set<wl_data_offer*> m_uriOffers; // ofertas que traen text/uri-list
    DropFn m_onDrop;
  };

} // namespace okaru::apps
