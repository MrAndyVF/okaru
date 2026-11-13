#pragma once

// OKARU: miniaturas de okaru-files según la especificación de freedesktop: caché compartida en
// ~/.cache/thumbnails/{normal,large} (la misma que usan Nautilus, Dolphin y el selector de archivos), con las
// etiquetas Thumb::URI y Thumb::MTime para saber si siguen al día. Se generan en hilos: imágenes con gdk-pixbuf,
// PDF con poppler y lo demás (video, audio, Office…) con los miniaturadores del sistema (/usr/share/thumbnailers).

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace okaru::files {

  struct FileEntry;

  // Ruta de la miniatura en la caché: <cache>/thumbnails/<normal|large>/<md5(uri)>.png
  [[nodiscard]] std::string thumbnailPath(const std::string& uri, bool large);
  // La miniatura existe y corresponde a esa URI y fecha de modificación.
  [[nodiscard]] bool thumbnailValid(const std::string& path, const std::string& uri, std::int64_t modified);

  // Un miniaturador del sistema (archivo .thumbnailer).
  struct ExternalThumbnailer {
    std::vector<std::string> mimeTypes;
    std::string exec; // con %i %o %s %u
  };
  [[nodiscard]] std::optional<ExternalThumbnailer> parseThumbnailer(const std::string& contents);
  // Línea de órdenes con los comodines reemplazados (se separa por espacios, como hace GNOME).
  [[nodiscard]] std::vector<std::string> expandThumbnailerExec(const std::string& exec, const std::string& path,
                                                               const std::string& uri, const std::string& output,
                                                               int size);

  class Thumbnailer {
  public:
    using ReadyFn = std::function<void()>;

    Thumbnailer();
    ~Thumbnailer();

    Thumbnailer(const Thumbnailer&) = delete;
    Thumbnailer& operator=(const Thumbnailer&) = delete;

    void setOnReady(ReadyFn callback) { m_onReady = std::move(callback); }
    // ¿Hay forma de hacerle miniatura a este archivo?
    [[nodiscard]] bool supports(const FileEntry& entry) const;
    // Ruta lista para mostrar, o nada (y se pide en segundo plano si hace falta).
    [[nodiscard]] std::optional<std::string> get(const FileEntry& entry, bool large);

  private:
    enum class State : std::uint8_t { Pending, Ready, Failed };
    struct Slot {
      std::int64_t modified = 0;
      State state = State::Pending;
      std::string path;
    };
    struct Job {
      std::string key;
      std::string uri;
      std::string path; // ruta local del archivo
      std::string contentType;
      std::int64_t modified = 0;
      bool large = false;
    };

    void worker();
    [[nodiscard]] bool generate(const Job& job, const std::string& output);
    [[nodiscard]] const ExternalThumbnailer* externalFor(const std::string& contentType) const;
    void loadExternal();

    std::vector<ExternalThumbnailer> m_external;
    std::unordered_map<std::string, Slot> m_slots; // solo hilo principal
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<Job> m_queue; // lo último pedido primero (lo que está a la vista)
    std::unordered_set<std::string> m_queued;
    std::vector<std::pair<std::string, Slot>> m_finished; // del hilo de trabajo al principal
    bool m_notifyPending = false;
    bool m_quit = false;
    std::vector<std::thread> m_threads;
    std::shared_ptr<int> m_alive = std::make_shared<int>(0);
    ReadyFn m_onReady;
  };

} // namespace okaru::files
