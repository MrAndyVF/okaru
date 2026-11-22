// OKARU: ThumbnailService::warmDiskCache deja la miniatura en la caché en disco sin cargarla.
#include "render/core/thumbnail_service.h"

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unistd.h>
#include <vector>
#include <webp/encode.h>

namespace fs = std::filesystem;

int main() {
  const fs::path root = fs::temp_directory_path() / ("okaru-thumb-warm-" + std::to_string(::getpid()));
  fs::remove_all(root);
  fs::create_directories(root / "cache");
  ::setenv("XDG_CACHE_HOME", (root / "cache").c_str(), 1);

  // Una imagen de origen de 64×48 (WebP, que el decodificador ya lee).
  std::vector<std::uint8_t> rgb(64 * 48 * 3, 128);
  std::uint8_t* encoded = nullptr;
  const std::size_t size = WebPEncodeRGB(rgb.data(), 64, 48, 64 * 3, 80.0F, &encoded);
  assert(encoded != nullptr && size > 0);
  std::ofstream(root / "source.webp", std::ios::binary).write(reinterpret_cast<const char*>(encoded), static_cast<std::streamsize>(size));
  WebPFree(encoded);

  const fs::path thumbs = root / "cache" / "okaru" / "thumbnails";
  const auto countThumbs = [&] {
    std::error_code ec;
    std::size_t n = 0;
    for (const auto& entry : fs::directory_iterator(thumbs, ec)) {
      n += entry.path().extension() == ".webp" ? 1 : 0;
    }
    return n;
  };
  {
    ThumbnailService service;
    service.warmDiskCache((root / "missing.png").string(), 32); // no existe: se ignora sin romper nada
    service.warmDiskCache((root / "source.webp").string(), 32);
    for (int i = 0; i < 200 && countThumbs() == 0; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(countThumbs() == 1);
    // Otra vez: ya está en disco, no se duplica.
    service.warmDiskCache((root / "source.webp").string(), 32);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    assert(countThumbs() == 1);
  }
  fs::remove_all(root);
  return 0;
}
