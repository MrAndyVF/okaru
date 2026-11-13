#pragma once

// OKARU: texturas de iconos de okaru-files. Resuelve los nombres que da GIO en el tema de iconos del sistema
// (Papirus, Adwaita…) y guarda una textura por archivo de icono y tamaño.

#include "apps/files/file_model.h"
#include "system/icon_resolver.h"
#include "render/core/texture_handle.h"

#include <string>
#include <unordered_map>

class Renderer;

namespace okaru::files {

  class IconCache {
  public:
    [[nodiscard]] TextureHandle get(Renderer& renderer, const FileEntry& entry, int pixels);
    [[nodiscard]] TextureHandle named(Renderer& renderer, const std::vector<std::string>& names, int pixels);
    // Tema cambiado: se vuelve a resolver todo.
    void clear(Renderer& renderer);

  private:
    IconResolver m_resolver{true};
    std::unordered_map<std::string, std::string> m_paths;      // nombres@tamaño → archivo
    std::unordered_map<std::string, TextureHandle> m_textures; // archivo@tamaño → textura
  };

} // namespace okaru::files
