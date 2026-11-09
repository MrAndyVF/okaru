#pragma once

// OKARU: "Abrir archivo" de las apps con el selector del portal de escritorio (org.freedesktop.portal.FileChooser),
// así se ve el mismo selector que en el resto del sistema.

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

class SessionBus;

namespace sdbus {
  class IProxy;
  class Variant;
} // namespace sdbus

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
    // "Guardar como": `name` propuesto y carpeta donde abrir; `done` recibe la ruta elegida (o nada).
    void save(const std::string& title, const std::string& name, const std::string& folder, Done done);

  private:
    void
    request(const char* method, const std::string& title, std::map<std::string, sdbus::Variant> options, Done done);

    SessionBus& m_bus;
    std::unique_ptr<sdbus::IProxy> m_request; // la respuesta llega como señal en este objeto
    unsigned m_token = 0;
  };

  // Convierte una URI file:// en ruta; vacío si no es file://.
  [[nodiscard]] std::string pathFromFileUri(std::string_view uri);

} // namespace okaru::apps
