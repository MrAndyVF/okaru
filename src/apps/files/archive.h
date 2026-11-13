#pragma once

// OKARU: comprimir y extraer en okaru-files con bsdtar (libarchive, ya está en todo Arch por pacman): zip, tar.gz,
// tar.xz, tar.zst y 7z para crear; además rar, iso y demás formatos de libarchive para extraer.

#include <string>
#include <vector>

namespace okaru::files {

  [[nodiscard]] bool isArchive(const std::string& contentType, const std::string& name);
  // Nombre sin la extensión del comprimido ("fotos.tar.gz" → "fotos").
  [[nodiscard]] std::string archiveStem(const std::string& name);

  // Síncronas (en un hilo). `names` son nombres dentro de `folder`; `output` la ruta del comprimido (la extensión
  // elige el formato).
  [[nodiscard]] bool compressFiles(const std::string& folder, const std::vector<std::string>& names,
                                   const std::string& output, std::string& error);
  // Extrae en una carpeta nueva con el nombre del comprimido; si adentro hay una sola cosa, la deja directo en
  // `folder` (sin carpeta doble). Devuelve la ruta de lo extraído.
  [[nodiscard]] bool extractArchive(const std::string& archive, const std::string& folder, std::string& result,
                                    std::string& error);

} // namespace okaru::files
