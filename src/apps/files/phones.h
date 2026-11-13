#pragma once

// OKARU: teléfonos de KDE Connect en okaru-files. Monta el almacenamiento por SFTP (plugin sftp del demonio de
// KDE Connect, que usa sshfs) y entra directo al almacenamiento interno. Llamadas D-Bus síncronas: en un hilo.

#include <string>
#include <utility>
#include <vector>

namespace okaru::files {

  struct Phone {
    std::string id;
    std::string name;
    bool mounted = false;
    std::vector<std::pair<std::string, std::string>> storages; // ruta local → nombre (interno, tarjeta SD)
  };

  // Teléfonos emparejados y a la vista con el plugin sftp. Vacía si KDE Connect no está corriendo (no lo arranca).
  [[nodiscard]] std::vector<Phone> listPhones();
  // Monta y devuelve la ruta del almacenamiento principal; espera a que el teléfono acepte (hasta 30 s).
  [[nodiscard]] bool mountPhone(const std::string& id, std::string& root, std::string& error);
  void unmountPhone(const std::string& id);

} // namespace okaru::files
