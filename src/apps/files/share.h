#pragma once

// OKARU: "Enviar a" de okaru-files. Destinos que haya en el equipo: LocalSend, teléfonos de KDE Connect,
// dispositivos Bluetooth emparejados (OBEX de BlueZ), Telegram y el correo predeterminado.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace okaru::files {

  enum class ShareKind : std::uint8_t { LocalSend, KdeConnect, Bluetooth, Telegram, Email };

  struct ShareTarget {
    ShareKind kind = ShareKind::LocalSend;
    std::string id; // KDE Connect: id del dispositivo; Bluetooth: dirección MAC
    std::string name;
    std::string glyph;
    [[nodiscard]] bool acceptsFolders() const noexcept { return kind == ShareKind::LocalSend; }
  };

  // Lenta (consulta kdeconnect-cli y bluetoothctl): llamar desde un hilo.
  [[nodiscard]] std::vector<ShareTarget> discoverShareTargets();

  // Envía rutas locales. `done` llega en el hilo principal con el resultado y un detalle (error o vacío).
  void shareFiles(const ShareTarget& target, std::vector<std::string> paths,
                  std::function<void(bool ok, std::string detail)> done);

  // "Device AA:BB:CC:DD:EE:FF Nombre" → (MAC, nombre); vacío si la línea no es de un dispositivo.
  [[nodiscard]] std::pair<std::string, std::string> parseBluetoothDevice(const std::string& line);

} // namespace okaru::files
