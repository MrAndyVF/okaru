#include "theme/apps_palette.h"

#include "render/core/color.h"
#include "util/file_utils.h"

#include <cmath>
#include <fstream>
#include <nlohmann/json.hpp>

namespace okaru::theme {

  std::filesystem::path appsPalettePath() { return std::filesystem::path(FileUtils::stateDir()) / "palette.json"; }

  bool writeAppsPalette(const AppsPalette& palette) {
    nlohmann::json tokens = nlohmann::json::object();
    for (const auto& [name, argb] : palette.tokens) {
      tokens[name] = formatRgbHex(rgbHex(argb & 0xFFFFFFU));
    }
    const nlohmann::json root{{"mode", palette.light ? "light" : "dark"}, {"tokens", std::move(tokens)}};

    const auto path = appsPalettePath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const auto tmp = std::filesystem::path(path.string() + ".tmp");
    {
      std::ofstream out(tmp, std::ios::trunc);
      if (!out) {
        return false;
      }
      out << root.dump(2) << '\n';
      if (!out) {
        return false;
      }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
      std::filesystem::remove(tmp, ec);
      return false;
    }
    return true;
  }

  std::optional<AppsPalette> readAppsPalette(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) {
      return std::nullopt;
    }
    const auto root = nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/false);
    if (!root.is_object() || !root.contains("tokens") || !root["tokens"].is_object()) {
      return std::nullopt;
    }
    AppsPalette palette;
    palette.light = root.value("mode", std::string()) == "light";
    for (const auto& [name, value] : root["tokens"].items()) {
      Color color;
      if (value.is_string() && tryParseHexColor(value.get<std::string>(), color)) {
        palette.tokens[name] = 0xFF000000U
            | (static_cast<std::uint32_t>(std::lround(color.r * 255.0F)) << 16U)
            | (static_cast<std::uint32_t>(std::lround(color.g * 255.0F)) << 8U)
            | static_cast<std::uint32_t>(std::lround(color.b * 255.0F));
      }
    }
    if (palette.tokens.empty()) {
      return std::nullopt;
    }
    return palette;
  }

} // namespace okaru::theme
