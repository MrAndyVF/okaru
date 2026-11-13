#pragma once

// OKARU: okaru-files como selector de archivos del sistema. `okaru-files --portal` es un backend de
// xdg-desktop-portal (org.freedesktop.impl.portal.FileChooser: OpenFile, SaveFile, SaveFiles); por cada pedido
// lanza `okaru-files --pick …` (una ventana en modo selector) y devuelve lo que imprime.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace okaru::files {

  enum class PickMode : std::uint8_t { Open, Save, Folder };

  struct PickFilter {
    std::string name;
    std::vector<std::string> globs; // "*.png"
    std::vector<std::string> mimes; // "image/png", "image/*"
  };

  struct PickerOptions {
    PickMode mode = PickMode::Open;
    bool multiple = false;
    std::string title;
    std::string folder; // carpeta inicial (ruta)
    std::string name;   // guardar: nombre propuesto
    std::string acceptLabel;
    std::vector<PickFilter> filters;
    std::size_t currentFilter = 0;
  };

  // Argumentos de la línea de órdenes del selector (ida y vuelta, para el portal y para probar a mano).
  [[nodiscard]] std::vector<std::string> pickerArguments(const PickerOptions& options);
  [[nodiscard]] PickerOptions parsePickerArguments(const std::vector<std::string>& args);

  // Bucle del servicio D-Bus; vuelve al perder el nombre en el bus.
  int runPortalService(const std::string& self);

} // namespace okaru::files
