// OKARU: núcleo de okaru-view (decodificación, carpeta, formatos).
#include "apps/viewer/viewer_image.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <print>
#include <unistd.h>

using namespace okaru::viewer;
namespace fs = std::filesystem;

int main() {
  const auto dir = fs::temp_directory_path() / ("okaru-viewer-" + std::to_string(::getpid()));
  fs::create_directories(dir);
  bool ok = true;
  const auto expect = [&](bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "okaru_viewer_image_test: {}", message);
      ok = false;
    }
  };

  // SVG 40×20: se rasteriza a su tamaño o al pedido, sin perder la proporción.
  std::ofstream(
      dir / "img10.svg"
  ) << R"(<svg xmlns="http://www.w3.org/2000/svg" width="40" height="20"><rect width="40" height="20" fill="red"/></svg>)";
  const auto svg = decodeImage(dir / "img10.svg", 16384);
  expect(svg && svg->vector && svg->width == 40 && svg->height == 20, "SVG a su tamaño");
  const auto bigSvg = decodeImage(dir / "img10.svg", 16384, 400);
  expect(bigSvg && bigSvg->width == 400 && bigSvg->height == 200, "SVG más grande");
  expect(bigSvg && bigSvg->frames.front().rgba[0] == 0xFF && bigSvg->frames.front().rgba[1] == 0, "SVG rojo");
  const auto capped = decodeImage(dir / "img10.svg", 100, 400);
  expect(capped && capped->width == 100, "respeta el lado máximo de textura");

  // PNG mínimo (1×1, opaco) escrito a mano.
  constexpr unsigned char kPng[] = {
      0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00,
      0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53, 0xDE, 0x00, 0x00, 0x00,
      0x0C, 0x49, 0x44, 0x41, 0x54, 0x08, 0xD7, 0x63, 0xF8, 0xCF, 0xC0, 0x00, 0x00, 0x03, 0x01, 0x01, 0x00, 0x18,
      0xDD, 0x8D, 0xB0, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82,
  };
  std::ofstream(dir / "img2.png", std::ios::binary).write(reinterpret_cast<const char*>(kPng), sizeof(kPng));
  const auto png = decodeImage(dir / "img2.png", 16384);
  expect(png && png->width == 1 && png->frames.size() == 1 && !png->vector, "PNG");
  std::ofstream(dir / "roto.jpg") << "no es una imagen";
  expect(!decodeImage(dir / "roto.jpg", 16384), "un archivo dañado da error, no se cae");
  std::ofstream(dir / "notas.txt") << "texto";

  const auto siblings = siblingImages(dir / "img2.png");
  expect(
      siblings.size() == 3 && siblings[0].filename() == "img2.png" && siblings[1].filename() == "img10.svg",
      "carpeta en orden natural y sin archivos que no son imágenes"
  );
  expect(formatName("foto.JPG") == "JPEG" && formatName("a.tif") == "TIFF" && formatName("x.cr2") == "CR2", "formatos");
  expect(isSupportedImage("a.HEIC") && isSupportedImage("b.nef") && !isSupportedImage("c.txt"), "extensiones");
  expect(cameraMetadata(dir / "img2.png").empty(), "sin EXIF no hay datos de cámara");

  fs::remove_all(dir);
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
