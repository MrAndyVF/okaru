#pragma once

// OKARU: motor de okaru-archive. libarchive para casi todo (listar, extraer, probar, crear y editar zip, tar.*, 7z,
// rar, iso…) y la línea de comandos de 7-Zip solo para lo que libarchive no hace: 7z/rar con contraseña o con la
// cabecera cifrada, crear 7z cifrados y partir en volúmenes. La contraseña va a 7-Zip por stdin, nunca en argv.
// Todo es síncrono: se llama desde un hilo.

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace okaru::archive {

  struct Item {
    std::string path; // dentro del comprimido, con '/' y sin '/' final
    std::uint64_t size = 0;
    std::int64_t modified = 0;
    std::uint32_t mode = 0;
    bool directory = false;
    bool encrypted = false;
  };

  enum class Status : std::uint8_t { Ok, NeedPassword, WrongPassword, Cancelled, Error };

  struct Result {
    Status status = Status::Ok;
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return status == Status::Ok; }
  };

  struct Progress {
    std::function<void(double)> update; // 0..1
    const std::atomic<bool>* cancel = nullptr;
  };

  // Formato por la extensión del nombre: "zip", "7z", "tar", "tar.gz", "tar.xz", "tar.bz2", "tar.zst" o "" si
  // okaru-archive no sabe escribirlo (rar, iso, cab… se abren en solo lectura).
  [[nodiscard]] std::string writableFormat(const std::string& name);
  [[nodiscard]] bool sevenZipAvailable();

  // Lista todo el contenido; las carpetas implícitas (a/b/c.txt sin entrada a/) se agregan.
  [[nodiscard]] Result list(const std::string& archive, const std::string& password, std::vector<Item>& items);

  // `paths`: elementos a extraer (una carpeta incluye su contenido); vacío = todo. `base`: carpeta del comprimido
  // que se quita del principio (extraer "docs/a.txt" estando en "docs" deja a.txt en `destination`).
  [[nodiscard]] Result extract(
      const std::string& archive, const std::string& password, const std::vector<std::string>& paths,
      const std::string& base, const std::string& destination, const Progress& progress
  );

  // Lee todos los datos para comprobar que el comprimido está sano.
  [[nodiscard]] Result test(const std::string& archive, const std::string& password, const Progress& progress);

  struct CreateOptions {
    int level = -1; // 0 (sin comprimir) .. 9 (máxima); -1 = la del formato
    std::string password;
    std::uint64_t volumeSize = 0; // bytes por parte; 0 = una sola
  };

  // `names`: nombres dentro de `folder`. El formato sale de la extensión de `output`.
  [[nodiscard]] Result create(
      const std::string& output, const std::string& folder, const std::vector<std::string>& names,
      const CreateOptions& options, const Progress& progress
  );

  struct Edit {
    std::vector<std::string> remove;                         // rutas (carpetas con su contenido)
    std::vector<std::pair<std::string, std::string>> rename; // de, a (ruta completa nueva)
    std::vector<std::string> folders;                        // carpetas nuevas
    std::vector<std::string> addFiles;                       // rutas locales a agregar…
    std::string addInto;                                     // …dentro de esta carpeta del comprimido
  };

  // Reescribe el comprimido con los cambios (a un temporal que luego lo reemplaza).
  [[nodiscard]] Result
  edit(const std::string& archive, const std::string& password, const Edit& changes, const Progress& progress);

  // Comentario de un zip (registro final del directorio central); nullopt si no es un zip legible.
  [[nodiscard]] std::optional<std::string> zipComment(const std::string& archive);
  [[nodiscard]] bool setZipComment(const std::string& archive, const std::string& comment, std::string& error);

} // namespace okaru::archive
