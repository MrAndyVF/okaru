#pragma once

// OKARU: "Abrir archivo" de las apps con el selector del portal de escritorio (org.freedesktop.portal.FileChooser),
// así se ve el mismo selector que en el resto del sistema.

#include <functional>
#include <memory>
#include <string>
#include <vector>

class SessionBus;

namespace sdbus {
  class IProxy;
}

namespace okaru::apps {

  struct FileFilter {
    std::string name;
    std::vector<std::string> mimeTypes; // "image/*", "video/mp4", …
  };

  class PortalFileChooser {
  public:
    using Done = std::function<void(std::vector<std::string> paths)>;

    explicit PortalFileChooser(SessionBus& bus);
    ~PortalFileChooser();

    // `done` llega en el hilo principal; vacío si se canceló o el portal no está.
    void open(const std::string& title, const std::vector<FileFilter>& filters, bool multiple, Done done);

  private:
    SessionBus& m_bus;
    std::unique_ptr<sdbus::IProxy> m_request; // la respuesta llega como señal en este objeto
    unsigned m_token = 0;
  };

  // Convierte una URI file:// en ruta; vacío si no es file://.
  [[nodiscard]] std::string pathFromFileUri(std::string_view uri);

} // namespace okaru::apps
