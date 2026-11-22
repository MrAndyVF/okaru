#include "shell/okaru_cursor_shake/okaru_xcursor.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>

namespace okaru::cursor_shake {

  namespace {

    constexpr std::uint32_t kImageType = 0xfffd0002U;
    constexpr int kMaxSide = 1024;

    std::uint32_t readU32(const std::vector<std::uint8_t>& data, std::size_t offset) {
      std::uint32_t value = 0;
      std::memcpy(&value, data.data() + offset, sizeof(value)); // Xcursor es little-endian, como x86/ARM
      return value;
    }

    std::vector<std::filesystem::path> iconDirs() {
      std::vector<std::filesystem::path> dirs;
      if (const char* env = std::getenv("XCURSOR_PATH"); env != nullptr && env[0] != '\0') {
        std::stringstream stream(env);
        for (std::string part; std::getline(stream, part, ':');) {
          if (part.starts_with("~/")) {
            if (const char* home = std::getenv("HOME"); home != nullptr) {
              part = std::string(home) + part.substr(1);
            }
          }
          if (!part.empty()) {
            dirs.emplace_back(part);
          }
        }
        return dirs;
      }
      if (const char* home = std::getenv("HOME"); home != nullptr) {
        dirs.emplace_back(std::filesystem::path(home) / ".local/share/icons");
        dirs.emplace_back(std::filesystem::path(home) / ".icons");
      }
      dirs.emplace_back("/usr/share/icons");
      dirs.emplace_back("/usr/share/pixmaps");
      return dirs;
    }

    std::vector<std::string> inheritedThemes(const std::filesystem::path& indexTheme) {
      std::ifstream file(indexTheme);
      std::vector<std::string> themes;
      for (std::string line; std::getline(file, line);) {
        if (!line.starts_with("Inherits=")) {
          continue;
        }
        std::stringstream stream(line.substr(9));
        for (std::string name; std::getline(stream, name, ',');) {
          std::erase_if(name, [](char c) { return c == ' ' || c == ';'; });
          if (!name.empty()) {
            themes.push_back(name);
          }
        }
      }
      return themes;
    }

    std::optional<CursorImage> findArrow(const std::string& theme, int depth) {
      if (depth > 4 || theme.empty()) {
        return std::nullopt;
      }
      const auto dirs = iconDirs();
      for (const auto& dir : dirs) {
        for (const char* name : {"left_ptr", "default", "arrow"}) {
          std::error_code ec;
          const auto path = dir / theme / "cursors" / name;
          if (std::filesystem::exists(path, ec)) {
            if (auto image = loadXcursorFile(path); image.has_value()) {
              return image;
            }
          }
        }
      }
      for (const auto& dir : dirs) {
        for (const auto& parent : inheritedThemes(dir / theme / "index.theme")) {
          if (parent != theme) {
            if (auto image = findArrow(parent, depth + 1); image.has_value()) {
              return image;
            }
          }
        }
      }
      return std::nullopt;
    }

  } // namespace

  std::optional<CursorImage> loadXcursorFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    const std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (data.size() < 16 || std::memcmp(data.data(), "Xcur", 4) != 0) {
      return std::nullopt;
    }
    const std::uint32_t headerSize = readU32(data, 4);
    const std::uint32_t count = readU32(data, 12);
    // La entrada de imagen con el tamaño nominal más grande (la primera que aparece: primer cuadro).
    std::uint32_t bestSize = 0;
    std::uint32_t bestPos = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
      const std::size_t entry = headerSize + static_cast<std::size_t>(i) * 12;
      if (entry + 12 > data.size()) {
        break;
      }
      if (readU32(data, entry) == kImageType && readU32(data, entry + 4) > bestSize) {
        bestSize = readU32(data, entry + 4);
        bestPos = readU32(data, entry + 8);
      }
    }
    if (bestSize == 0 || static_cast<std::size_t>(bestPos) + 36 > data.size()) {
      return std::nullopt;
    }
    const auto width = static_cast<int>(readU32(data, bestPos + 16));
    const auto height = static_cast<int>(readU32(data, bestPos + 20));
    if (width <= 0 || height <= 0 || width > kMaxSide || height > kMaxSide) {
      return std::nullopt;
    }
    const std::size_t pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    const std::size_t start = bestPos + 36;
    if (start + pixels * 4 > data.size()) {
      return std::nullopt;
    }
    CursorImage image{
        .width = width,
        .height = height,
        .hotX = static_cast<int>(readU32(data, bestPos + 24)),
        .hotY = static_cast<int>(readU32(data, bestPos + 28)),
        .nominalSize = static_cast<int>(bestSize),
    };
    image.rgba.resize(pixels * 4);
    for (std::size_t i = 0; i < pixels; ++i) {
      // ARGB premultiplicado → RGBA sin premultiplicar.
      const std::uint32_t argb = readU32(data, start + i * 4);
      const std::uint32_t a = argb >> 24U;
      const auto unpremultiply = [a](std::uint32_t c) {
        return static_cast<std::uint8_t>(a == 0 ? 0 : std::min<std::uint32_t>(255, (c * 255 + a / 2) / a));
      };
      image.rgba[i * 4 + 0] = unpremultiply((argb >> 16U) & 0xffU);
      image.rgba[i * 4 + 1] = unpremultiply((argb >> 8U) & 0xffU);
      image.rgba[i * 4 + 2] = unpremultiply(argb & 0xffU);
      image.rgba[i * 4 + 3] = static_cast<std::uint8_t>(a);
    }
    return image;
  }

  std::optional<CursorImage> loadThemeArrow(const std::string& theme) {
    if (auto image = findArrow(theme, 0); image.has_value()) {
      return image;
    }
    return theme == "default" ? std::nullopt : findArrow("default", 0);
  }

} // namespace okaru::cursor_shake
