#include "apps/files/icon_cache.h"

#include "render/core/renderer.h"
#include "render/core/texture_manager.h"

#include <array>

namespace okaru::files {

  namespace {

    // Pocos tamaños distintos: menos texturas y el SVG se rasteriza nítido.
    int bucket(int pixels) {
      static constexpr std::array<int, 9> kSizes{16, 24, 32, 48, 64, 96, 128, 192, 256};
      for (const int size : kSizes) {
        if (pixels <= size) {
          return size;
        }
      }
      return kSizes.back();
    }

  } // namespace

  TextureHandle IconCache::named(Renderer& renderer, const std::vector<std::string>& names, int pixels) {
    const int size = bucket(pixels);
    std::string key;
    for (const auto& name : names) {
      key += name;
      key += '|';
    }
    key += std::to_string(size);
    auto found = m_paths.find(key);
    if (found == m_paths.end()) {
      std::string path;
      for (const auto& name : names) {
        path = m_resolver.resolve(name, size);
        if (!path.empty()) {
          break;
        }
      }
      found = m_paths.emplace(key, path).first;
    }
    if (found->second.empty()) {
      return {};
    }
    const std::string textureKey = found->second + "@" + std::to_string(size);
    if (const auto it = m_textures.find(textureKey); it != m_textures.end()) {
      return it->second;
    }
    TextureHandle handle = renderer.textureManager().loadFromFile(found->second, size);
    m_textures.emplace(textureKey, handle);
    return handle;
  }

  TextureHandle IconCache::get(Renderer& renderer, const FileEntry& entry, int pixels) {
    return named(renderer, entry.icons, pixels);
  }

  void IconCache::clear(Renderer& renderer) {
    for (auto& [key, handle] : m_textures) {
      if (handle.valid()) {
        renderer.textureManager().unload(handle);
      }
    }
    m_textures.clear();
    m_paths.clear();
  }

} // namespace okaru::files
