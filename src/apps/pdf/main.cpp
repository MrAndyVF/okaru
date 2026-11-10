// OKARU: okaru-pdf, el lector de PDF de OKARU.
#include "apps/common/okaru_app.h"
#include "apps/common/portal_file_chooser.h"
#include "apps/pdf/pdf_window.h"

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
      .appId = "dev.okaru.Pdf",
      .titleKey = "apps.pdf.title",
      .width = 900.0F,
      .height = 1000.0F,
      .minWidth = 520.0F,
      .minHeight = 360.0F,
  });
  okaru::pdf::PdfWindow window(app, files);
  return app.run();
}
