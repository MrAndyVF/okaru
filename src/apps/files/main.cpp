// OKARU: okaru-files, el explorador de archivos de OKARU.
#include "apps/common/okaru_app.h"
#include "apps/files/file_model.h"
#include "apps/files/files_window.h"

#include <clocale>
#include <string>
#include <vector>

int main(int argc, char* argv[]) {
  std::setlocale(LC_ALL, "");
  std::setlocale(LC_NUMERIC, "C");

  // Rutas, URIs (file://, smb://, sftp://…) o nada (carpeta personal); cada una en su pestaña.
  std::vector<std::string> locations;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (!arg.starts_with("-")) {
      locations.push_back(okaru::files::pathToUri(arg));
    }
  }

  okaru::apps::OkaruApp app({
      .appId = "dev.okaru.Files",
      .titleKey = "apps.files.title",
      .width = 1200.0F,
      .height = 760.0F,
      .minWidth = 420.0F,
      .minHeight = 360.0F,
  });
  okaru::files::FilesWindow window(app, locations);
  return app.run();
}
