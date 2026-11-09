// OKARU: paleta compartida entre la shell y sus apps (palette.json).
#include "theme/apps_palette.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <print>
#include <unistd.h>

int main() {
  const auto dir = std::filesystem::temp_directory_path() / ("okaru-apps-palette-" + std::to_string(::getpid()));
  ::setenv("XDG_STATE_HOME", dir.c_str(), 1);
  bool ok = true;
  const auto expect = [&](bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "okaru_apps_palette_test: {}", message);
      ok = false;
    }
  };

  const okaru::theme::AppsPalette written{.tokens = {{"primary", 0xFF7AA2F7U}, {"surface", 0x1A1B26U}}, .light = true};
  expect(okaru::theme::writeAppsPalette(written), "escribe palette.json");
  expect(okaru::theme::appsPalettePath().parent_path() == dir / "okaru", "vive en el estado de OKARU");
  const auto read = okaru::theme::readAppsPalette();
  expect(read.has_value() && read->light, "vuelve el modo claro");
  expect(read.has_value() && read->tokens.at("primary") == 0xFF7AA2F7U, "color con alfa opaco");
  expect(read.has_value() && read->tokens.at("surface") == 0xFF1A1B26U, "sin alfa se lee opaco");

  std::ofstream(okaru::theme::appsPalettePath()) << "{ roto";
  expect(!okaru::theme::readAppsPalette().has_value(), "un archivo roto no rompe la app");
  expect(!okaru::theme::readAppsPalette(dir / "no-existe.json").has_value(), "sin archivo, paleta de fábrica");

  std::filesystem::remove_all(dir);
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
