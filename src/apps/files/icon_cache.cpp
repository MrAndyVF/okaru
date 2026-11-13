#include "apps/files/icon_cache.h"

#include "render/core/renderer.h"
#include "render/core/texture_manager.h"

#include <algorithm>
#include <cmath>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>

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

    constexpr Color rgb(int hex) {
      return {
          static_cast<float>((hex >> 16) & 0xFF) / 255.0F, static_cast<float>((hex >> 8) & 0xFF) / 255.0F,
          static_cast<float>(hex & 0xFF) / 255.0F, 1.0F
      };
    }

    struct Hsl {
      float h = 0.0F; // 0..1
      float s = 0.0F;
      float l = 0.0F;
    };

    Hsl toHsl(float r, float g, float b) {
      const float mx = std::max({r, g, b});
      const float mn = std::min({r, g, b});
      Hsl out{0.0F, 0.0F, (mx + mn) * 0.5F};
      const float d = mx - mn;
      if (d < 1e-5F) {
        return out;
      }
      out.s = out.l > 0.5F ? d / (2.0F - mx - mn) : d / (mx + mn);
      float h = mx == r ? (g - b) / d + (g < b ? 6.0F : 0.0F) : mx == g ? (b - r) / d + 2.0F : (r - g) / d + 4.0F;
      out.h = h / 6.0F;
      return out;
    }

    float hueChannel(float p, float q, float t) {
      t = t < 0.0F ? t + 1.0F : t > 1.0F ? t - 1.0F : t;
      if (t < 1.0F / 6.0F) {
        return p + (q - p) * 6.0F * t;
      }
      if (t < 0.5F) {
        return q;
      }
      if (t < 2.0F / 3.0F) {
        return p + (q - p) * (2.0F / 3.0F - t) * 6.0F;
      }
      return p;
    }

    void fromHsl(const Hsl& c, float& r, float& g, float& b) {
      if (c.s < 1e-5F) {
        r = g = b = c.l;
        return;
      }
      const float q = c.l < 0.5F ? c.l * (1.0F + c.s) : c.l + c.s - c.l * c.s;
      const float p = 2.0F * c.l - q;
      r = hueChannel(p, q, c.h + 1.0F / 3.0F);
      g = hueChannel(p, q, c.h);
      b = hueChannel(p, q, c.h - 1.0F / 3.0F);
    }

  } // namespace

  const std::vector<FolderColorOption>& folderColorOptions() {
    // Mismos nombres que las variantes de Papirus; el color sirve para la muestra y para otros temas.
    static const std::vector<FolderColorOption> options{
        {"red", "apps.files.color.red", rgb(0xE25252)},
        {"deeporange", "apps.files.color.deeporange", rgb(0xEB6637)},
        {"orange", "apps.files.color.orange", rgb(0xEE923A)},
        {"yellow", "apps.files.color.yellow", rgb(0xF9BD30)},
        {"green", "apps.files.color.green", rgb(0x87B158)},
        {"teal", "apps.files.color.teal", rgb(0x16A085)},
        {"cyan", "apps.files.color.cyan", rgb(0x00BCD4)},
        {"blue", "apps.files.color.blue", rgb(0x5294E2)},
        {"indigo", "apps.files.color.indigo", rgb(0x5C6BC0)},
        {"violet", "apps.files.color.violet", rgb(0x7E57C2)},
        {"magenta", "apps.files.color.magenta", rgb(0xCA71DF)},
        {"pink", "apps.files.color.pink", rgb(0xF06292)},
        {"brown", "apps.files.color.brown", rgb(0xAE8E6C)},
        {"grey", "apps.files.color.grey", rgb(0x8E8E8E)},
        {"black", "apps.files.color.black", rgb(0x4F4F4F)},
        {"white", "apps.files.color.white", rgb(0xE4E4E4)},
    };
    return options;
  }

  std::string coloredFolderIconName(std::string_view icon, std::string_view color) {
    // folder → folder-red; folder-documents → folder-red-documents; user-home → user-red-home.
    for (const std::string_view prefix : {std::string_view("folder"), std::string_view("user")}) {
      if (icon.starts_with(prefix) && (icon.size() == prefix.size() || icon[prefix.size()] == '-')) {
        std::string out(prefix);
        out += '-';
        out += color;
        out += icon.substr(prefix.size());
        return out;
      }
    }
    return {};
  }

  void recolorRgba(std::uint8_t* rgba, int width, int height, const Color& target) {
    const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    // Color dominante del dibujo (solo píxeles con color y bastante opacos).
    float sumX = 0.0F;
    float sumY = 0.0F;
    float sumS = 0.0F;
    float sumL = 0.0F;
    std::size_t n = 0;
    for (std::size_t i = 0; i < count; ++i) {
      const std::uint8_t* p = rgba + i * 4;
      if (p[3] < 128) {
        continue;
      }
      const Hsl c = toHsl(p[0] / 255.0F, p[1] / 255.0F, p[2] / 255.0F);
      if (c.s < 0.2F) {
        continue;
      }
      sumX += std::cos(c.h * 6.2831853F);
      sumY += std::sin(c.h * 6.2831853F);
      sumS += c.s;
      sumL += c.l;
      ++n;
    }
    if (n == 0) {
      return; // dibujo gris: nada que recolorear
    }
    const float baseS = sumS / static_cast<float>(n);
    const float baseL = sumL / static_cast<float>(n);
    const Hsl goal = toHsl(target.r, target.g, target.b);
    for (std::size_t i = 0; i < count; ++i) {
      std::uint8_t* p = rgba + i * 4;
      if (p[3] == 0) {
        continue;
      }
      Hsl c = toHsl(p[0] / 255.0F, p[1] / 255.0F, p[2] / 255.0F);
      if (c.s < 0.12F) {
        continue; // blancos, grises y negros del dibujo se quedan como están
      }
      c.h = goal.h;
      c.s = std::clamp(c.s * goal.s / std::max(baseS, 0.01F), 0.0F, 1.0F);
      c.l = std::clamp(c.l + (goal.l - baseL), 0.0F, 1.0F);
      float r = 0.0F;
      float g = 0.0F;
      float b = 0.0F;
      fromHsl(c, r, g, b);
      p[0] = static_cast<std::uint8_t>(std::lround(r * 255.0F));
      p[1] = static_cast<std::uint8_t>(std::lround(g * 255.0F));
      p[2] = static_cast<std::uint8_t>(std::lround(b * 255.0F));
    }
  }

  std::string IconCache::resolvePath(const std::vector<std::string>& names, int size) {
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
    return found->second;
  }

  TextureHandle IconCache::file(Renderer& renderer, const std::string& path, int pixels) {
    if (path.empty()) {
      return {};
    }
    const int size = bucket(pixels);
    const std::string textureKey = path + "@" + std::to_string(size);
    if (const auto it = m_textures.find(textureKey); it != m_textures.end()) {
      return it->second;
    }
    TextureHandle handle = renderer.textureManager().loadFromFile(path, size);
    m_textures.emplace(textureKey, handle);
    return handle;
  }

  TextureHandle IconCache::named(Renderer& renderer, const std::vector<std::string>& names, int pixels) {
    const int size = bucket(pixels);
    return file(renderer, resolvePath(names, size), size);
  }

  void IconCache::setAccent(const Color& accent) { m_accent = accent; }

  TextureHandle IconCache::folder(Renderer& renderer, const FileEntry& entry, const std::string& color, int size) {
    // 1) La variante del tema (Papirus y derivados): idéntica a papirus-folders.
    if (color != "accent" && !color.starts_with('#')) {
      std::vector<std::string> variants;
      for (const auto& icon : entry.icons) {
        if (std::string variant = coloredFolderIconName(icon, color); !variant.empty()) {
          variants.push_back(std::move(variant));
        }
      }
      if (const std::string path = resolvePath(variants, size); !path.empty()) {
        return file(renderer, path, size);
      }
    }
    // 2) Recolorear el icono del tema.
    Color target = m_accent;
    if (color.starts_with('#') && color.size() == 7) {
      const int hex = static_cast<int>(std::strtol(color.c_str() + 1, nullptr, 16));
      target = rgb(hex);
    } else if (color != "accent") {
      const auto& options = folderColorOptions();
      const auto it = std::ranges::find_if(options, [&](const FolderColorOption& o) { return color == o.id; });
      if (it == options.end()) {
        return named(renderer, entry.icons, size);
      }
      target = it->color;
    }
    const std::string base = resolvePath(entry.icons, size);
    if (base.empty()) {
      return {};
    }
    char suffix[32];
    std::snprintf(
        suffix, sizeof(suffix), "@%d#%02x%02x%02x", size, static_cast<int>(target.r * 255),
        static_cast<int>(target.g * 255), static_cast<int>(target.b * 255)
    );
    const std::string key = base + suffix;
    if (const auto it = m_textures.find(key); it != m_textures.end()) {
      return it->second;
    }
    TextureHandle handle;
    if (GdkPixbuf* loaded = gdk_pixbuf_new_from_file_at_size(base.c_str(), size, size, nullptr); loaded != nullptr) {
      GdkPixbuf* withAlpha = gdk_pixbuf_add_alpha(loaded, FALSE, 0, 0, 0);
      g_object_unref(loaded);
      const int w = gdk_pixbuf_get_width(withAlpha);
      const int h = gdk_pixbuf_get_height(withAlpha);
      const int stride = gdk_pixbuf_get_rowstride(withAlpha);
      std::vector<std::uint8_t> pixels(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
      const guchar* src = gdk_pixbuf_get_pixels(withAlpha);
      for (int y = 0; y < h; ++y) {
        std::copy_n(src + y * stride, w * 4, pixels.data() + static_cast<std::size_t>(y) * w * 4);
      }
      g_object_unref(withAlpha);
      recolorRgba(pixels.data(), w, h, target);
      handle = renderer.textureManager().loadFromRgba(pixels.data(), w, h);
    }
    m_textures.emplace(key, handle);
    return handle;
  }

  TextureHandle IconCache::get(Renderer& renderer, const FileEntry& entry, int pixels) {
    if (!entry.customIcon.empty()) {
      if (const TextureHandle handle = file(renderer, entry.customIcon, pixels); handle.valid()) {
        return handle;
      }
    }
    if (!entry.customIconName.empty()) {
      std::vector<std::string> names{entry.customIconName};
      names.insert(names.end(), entry.icons.begin(), entry.icons.end());
      return named(renderer, names, pixels);
    }
    if (entry.directory && !entry.mountable) {
      const std::string& color = entry.folderColor.empty() ? m_defaultFolderColor : entry.folderColor;
      if (!color.empty()) {
        return folder(renderer, entry, color, bucket(pixels));
      }
    }
    return named(renderer, entry.icons, pixels);
  }

  std::string IconCache::pathFor(const FileEntry& entry, int pixels) {
    if (!entry.customIcon.empty()) {
      return entry.customIcon;
    }
    std::vector<std::string> names = entry.icons;
    if (!entry.customIconName.empty()) {
      names.insert(names.begin(), entry.customIconName);
    } else if (entry.directory && !entry.mountable) {
      const std::string& color = entry.folderColor.empty() ? m_defaultFolderColor : entry.folderColor;
      if (!color.empty() && color != "accent" && !color.starts_with('#')) {
        std::vector<std::string> variants;
        for (const auto& icon : entry.icons) {
          if (std::string variant = coloredFolderIconName(icon, color); !variant.empty()) {
            variants.push_back(std::move(variant));
          }
        }
        names.insert(names.begin(), variants.begin(), variants.end());
      }
    }
    return resolvePath(names, pixels);
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
