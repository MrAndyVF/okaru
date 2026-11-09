// OKARU: okaru-play, el reproductor de audio y video de OKARU.
#include "apps/common/okaru_app.h"
#include "apps/common/portal_file_chooser.h"
#include "apps/player/player_window.h"

#include <clocale>
#include <filesystem>
#include <string_view>
#include <vector>

int main(int argc, char* argv[]) {
  std::setlocale(LC_ALL, "");
  std::setlocale(LC_NUMERIC, "C"); // mpv exige LC_NUMERIC=C

  // Rutas, URIs file:// o URLs (http…, que mpv abre directamente).
  std::vector<std::filesystem::path> files;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    files.emplace_back(arg.starts_with("file://") ? okaru::apps::pathFromFileUri(arg) : std::string(arg));
  }

  okaru::apps::OkaruApp app({
      .appId = "dev.okaru.Player",
      .titleKey = "apps.play.title",
      .width = 1100.0F,
      .height = 700.0F,
      .minWidth = 560.0F,
      .minHeight = 360.0F,
  });
  okaru::player::PlayerWindow window(app, files);
  return app.run();
}
