// OKARU: okaru-archive, el gestor de comprimidos de OKARU.
//   okaru-archive [comprimido]         abre un comprimido (o la pantalla de inicio)
//   okaru-archive --create archivos…   comprimidos nuevos con esos archivos
#include "apps/archive/archive_window.h"
#include "apps/common/okaru_app.h"
#include "apps/common/portal_file_chooser.h"

#include <clocale>
#include <string>
#include <string_view>
#include <vector>

int main(int argc, char* argv[]) {
  std::setlocale(LC_ALL, "");
  std::setlocale(LC_NUMERIC, "C");

  std::string archive;
  std::vector<std::string> create;
  bool creating = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--create") {
      creating = true;
      continue;
    }
    // Rutas o URIs file:// (los exploradores pasan cualquiera de las dos).
    std::string path = arg.starts_with("file://") ? okaru::apps::pathFromFileUri(arg) : std::string(arg);
    if (creating) {
      create.push_back(std::move(path));
    } else if (archive.empty()) {
      archive = std::move(path);
    }
  }

  okaru::apps::OkaruApp app({
      .appId = "dev.okaru.Archive",
      .titleKey = "apps.archive.title",
      .width = 900.0F,
      .height = 620.0F,
      .minWidth = 520.0F,
      .minHeight = 360.0F,
  });
  okaru::archive::ArchiveWindow window(app, archive, create);
  return app.run();
}
