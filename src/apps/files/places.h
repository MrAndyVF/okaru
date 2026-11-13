#pragma once

// OKARU: lugares del panel lateral de okaru-files: carpetas XDG, marcadores (los de GTK, compartidos con Nautilus y
// el selector de archivos), unidades y montajes de GVolumeMonitor (USB, discos, MTP, red de gvfs), papelera y red.

#include "apps/files/phones.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

using GVolumeMonitor = struct _GVolumeMonitor;

namespace okaru::files {

  enum class PlaceSection : std::uint8_t { Places, Bookmarks, Devices, Network };

  struct Place {
    PlaceSection section = PlaceSection::Places;
    std::string name;
    std::string uri;   // vacío si hay que montarlo primero
    std::string glyph; // glifo Tabler del panel
    std::string id;    // identificador estable (volumen/montaje) para montar y expulsar
    bool mounted = true;
    bool canUnmount = false;
    bool canEject = false;
    std::uint64_t total = 0; // capacidad (montajes locales), 0 si no se sabe
    std::uint64_t free = 0;
  };

  class Places {
  public:
    using ChangedFn = std::function<void()>;
    using DoneFn = std::function<void(bool ok, std::string uriOrError)>;

    Places();
    ~Places();

    Places(const Places&) = delete;
    Places& operator=(const Places&) = delete;

    void setOnChanged(ChangedFn callback) { m_onChanged = std::move(callback); }
    void refresh();

    [[nodiscard]] const std::vector<Place>& places() const noexcept { return m_places; }

    [[nodiscard]] bool isBookmarked(const std::string& uri) const;
    void addBookmark(const std::string& uri);
    void removeBookmark(const std::string& uri);
    void renameBookmark(const std::string& uri, const std::string& name);

    // Teléfonos de KDE Connect (se consultan en un hilo; aquí solo se guardan).
    void setPhones(std::vector<Phone> phones);

    // Monta un volumen (`id` de Place) y devuelve la URI de su raíz. `operation` (GMountOperation*) pide
    // contraseñas o frases de paso (LUKS) con el diálogo de la ventana.
    void mount(const std::string& id, void* operation, DoneFn done);
    void unmount(const std::string& id, bool eject, void* operation, DoneFn done);

  private:
    struct Bookmark {
      std::string uri;
      std::string name;
    };
    [[nodiscard]] std::vector<Bookmark> readBookmarks() const;
    void writeBookmarks(const std::vector<Bookmark>& bookmarks) const;
    static void onMonitorChanged(void* monitor, void* object, void* self);

    GVolumeMonitor* m_monitor = nullptr;
    std::vector<Place> m_places;
    std::vector<Phone> m_phones;
    ChangedFn m_onChanged;
  };

} // namespace okaru::files
