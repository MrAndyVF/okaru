// OKARU: okaru-edit, el editor de texto de OKARU.
#include "apps/common/okaru_app.h"
#include "apps/common/portal_file_chooser.h"
#include "apps/editor/editor_window.h"

#include <clocale>
#include <filesystem>
#include <string_view>
#include <vector>

int main(int argc, char* argv[]) {
  std::setlocale(LC_ALL, "");
  std::setlocale(LC_NUMERIC, "C");

  // Rutas o URIs file:// (los exploradores pasan cualquiera de las dos).
  std::vector<std::filesystem::path> files;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    files.emplace_back(arg.starts_with("file://") ? okaru::apps::pathFromFileUri(arg) : std::string(arg));
  }

  okaru::apps::OkaruApp app({
      .appId = "dev.okaru.Editor",
      .dataName = "editor",
      .titleKey = "apps.edit.title",
      .width = 1000.0F,
      .height = 760.0F,
      .minWidth = 520.0F,
      .minHeight = 360.0F,
  });
  okaru::editor::EditorWindow window(app, files);
  return app.run();
}
