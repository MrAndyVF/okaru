#pragma once

// OKARU: texturas de iconos de okaru-files. Resuelve los nombres que da GIO en el tema de iconos del sistema
// (Papirus, Adwaita…) y guarda una textura por archivo de icono y tamaño. También pinta las carpetas: si el tema
// trae la variante de color (Papirus: folder-red, folder-red-documents…) se usa esa; si no, se recolorea el icono.

#include "apps/files/file_model.h"
#include "render/core/color.h"
#include "render/core/texture_handle.h"
#include "system/icon_resolver.h"

#include <array>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class Renderer;

namespace okaru::files {

  struct FolderColorOption {
    const char* id;    // nombre de la variante de Papirus ("blue" → folder-blue)
    const char* label; // clave i18n
    Color color;       // para la muestra y para recolorear
  };

  // Colores de carpeta que se ofrecen (además de "tema" = sin cambio y "accent" = color primario de OKARU).
  [[nodiscard]] const std::vector<FolderColorOption>& folderColorOptions();
  // Variante de color de un nombre de icono de carpeta: ("folder-documents", "red") → "folder-red-documents".
  [[nodiscard]] std::string coloredFolderIconName(std::string_view icon, std::string_view color);
  // Recolorea píxeles RGBA (sin premultiplicar) hacia `target`, conservando luces y sombras del dibujo.
  void recolorRgba(std::uint8_t* rgba, int width, int height, const Color& target);

  class IconCache {
  public:
    [[nodiscard]] TextureHandle get(Renderer& renderer, const FileEntry& entry, int pixels);
    [[nodiscard]] TextureHandle named(Renderer& renderer, const std::vector<std::string>& names, int pixels);
    // Una imagen cualquiera (icono propio, miniatura) a ese tamaño.
    [[nodiscard]] TextureHandle file(Renderer& renderer, const std::string& path, int pixels);

    // "" = el del tema, "accent" = primario de OKARU, o un id de folderColorOptions().
    void setDefaultFolderColor(std::string color) { m_defaultFolderColor = std::move(color); }
    [[nodiscard]] const std::string& defaultFolderColor() const noexcept { return m_defaultFolderColor; }
    void setAccent(const Color& accent);
    // Tema o paleta cambiados: se vuelve a resolver todo.
    void clear(Renderer& renderer);

  private:
    [[nodiscard]] std::string resolvePath(const std::vector<std::string>& names, int size);
    [[nodiscard]] TextureHandle folder(Renderer& renderer, const FileEntry& entry, const std::string& color, int size);

    IconResolver m_resolver{true};
    std::unordered_map<std::string, std::string> m_paths;      // nombres@tamaño → archivo
    std::unordered_map<std::string, TextureHandle> m_textures; // archivo@tamaño(@color) → textura
    std::string m_defaultFolderColor;
    Color m_accent{0.32F, 0.58F, 0.89F, 1.0F};
  };

} // namespace okaru::files
