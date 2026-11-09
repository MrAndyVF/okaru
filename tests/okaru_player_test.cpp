// OKARU: utilidades de okaru-play (tiempo, tipos de archivo, lista de la carpeta).
#include "apps/player/mpv_player.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <print>
#include <unistd.h>

using namespace okaru::player;
namespace fs = std::filesystem;

int main() {
  bool ok = true;
  const auto expect = [&](bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "okaru_player_test: {}", message);
      ok = false;
    }
  };
  expect(formatTime(0) == "0:00" && formatTime(65.9) == "1:05" && formatTime(3723) == "1:02:03", "formato de tiempo");
  expect(formatTime(-3) == "0:00", "sin tiempos negativos");
  expect(isMediaFile("a.MKV") && isMediaFile("b.flac") && !isMediaFile("c.srt") && !isMediaFile("d"), "tipos");

  const auto dir = fs::temp_directory_path() / ("okaru-player-" + std::to_string(::getpid()));
  fs::create_directories(dir);
  for (const char* name : {"cap10.mp4", "cap2.mp4", "cap2.srt", "tema.mp3"}) {
    std::ofstream(dir / name) << "x";
  }
  const auto list = siblingMedia(dir / "cap2.mp4");
  expect(
      list.size() == 3 && list[0].filename() == "cap2.mp4" && list[1].filename() == "cap10.mp4",
      "carpeta en orden natural, sin subtítulos"
  );
  fs::remove_all(dir);

  PlayerState state;
  state.tracks = {{.id = 1, .type = "audio", .selected = true}};
  expect(!state.hasVideo(), "solo audio");
  state.tracks.push_back({.id = 1, .type = "video", .selected = true});
  expect(state.hasVideo(), "con video");
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
