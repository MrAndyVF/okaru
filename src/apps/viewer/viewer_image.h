#pragma once

// OKARU: decodificación y metadatos de okaru-view. Usa los decodificadores de la shell (PNG, JPEG, GIF, WebP, JXL,
// HEIF/AVIF, BMP, ICO, QOI, TGA, PNM, SVG), anima GIF y WebP, y recurre a gdk-pixbuf (TIFF, XPM, ICNS…) y a
// libraw (RAW de cámara) para el resto.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace okaru::viewer {

  struct Frame {
    std::vector<std::uint8_t> rgba; // RGBA8 sin premultiplicar, width*height*4
    std::uint32_t durationMs = 0;
  };

  struct DecodedImage {
    int width = 0; // tamaño de los cuadros (puede venir reducido a maxSize)
    int height = 0;
    int sourceWidth = 0; // tamaño real de la imagen (en un SVG rasterizado a otro tamaño, ese tamaño)
    int sourceHeight = 0;
    std::vector<Frame> frames; // más de uno = animada
    bool vector = false;       // SVG: se puede volver a rasterizar más grande
    bool truncated = false;    // animación recortada por el límite de memoria
  };

  [[nodiscard]] bool isSupportedImage(const std::filesystem::path& path);
  // Imágenes de la misma carpeta, en orden natural (img2 antes que img10), como un explorador de archivos.
  [[nodiscard]] std::vector<std::filesystem::path> siblingImages(const std::filesystem::path& file);
  // `maxSize` = lado máximo de la textura; `svgSize` = lado al que rasterizar un SVG (0 = su tamaño propio).
  [[nodiscard]] std::expected<DecodedImage, std::string>
  decodeImage(const std::filesystem::path& path, int maxSize, int svgSize = 0);
  // "PNG", "JPEG", "CR2"… a partir de la extensión.
  [[nodiscard]] std::string formatName(const std::filesystem::path& path);

  struct MetadataField {
    std::string key; // clave i18n bajo apps.view.properties.*
    std::string value;
  };
  // Datos de cámara (EXIF) con exiv2; vacío si la imagen no trae.
  [[nodiscard]] std::vector<MetadataField> cameraMetadata(const std::filesystem::path& path);

} // namespace okaru::viewer
