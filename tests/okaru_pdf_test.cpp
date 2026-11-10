// OKARU: núcleo de okaru-pdf. El PDF de prueba se genera con cairo: tres páginas, texto, un enlace interno, uno
// externo y un índice.
#include "apps/pdf/pdf_document.h"

#include <cairo-pdf.h>
#include <cairo.h>
#include <cstdlib>
#include <filesystem>
#include <print>
#include <unistd.h>

using namespace okaru::pdf;
namespace fs = std::filesystem;

namespace {
  bool g_ok = true;
  void expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "okaru_pdf_test: {}", message);
      g_ok = false;
    }
  }

  void writeSample(const fs::path& path) {
    cairo_surface_t* surface = cairo_pdf_surface_create(path.c_str(), 400, 600); // puntos
    cairo_pdf_surface_set_metadata(surface, CAIRO_PDF_METADATA_TITLE, "Documento de prueba");
    cairo_pdf_surface_set_metadata(surface, CAIRO_PDF_METADATA_AUTHOR, "OKARU");
    cairo_t* cr = cairo_create(surface);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 20);
    for (int page = 0; page < 3; ++page) {
      cairo_pdf_surface_add_outline(
          surface, CAIRO_PDF_OUTLINE_ROOT,
          page == 0       ? "Capítulo uno"
              : page == 1 ? "Capítulo dos"
                          : "Final",
          ("page=" + std::to_string(page + 1)).c_str(), CAIRO_PDF_OUTLINE_FLAG_OPEN
      );
      cairo_move_to(cr, 50, 100);
      cairo_show_text(cr, page == 1 ? "Hola mundo OKARU" : "Texto de relleno");
      if (page == 0) {
        cairo_tag_begin(cr, CAIRO_TAG_LINK, "page=3 pos=[50 100]");
        cairo_move_to(cr, 50, 200);
        cairo_show_text(cr, "Ir al final");
        cairo_tag_end(cr, CAIRO_TAG_LINK);
        cairo_tag_begin(cr, CAIRO_TAG_LINK, "uri='https://example.org'");
        cairo_move_to(cr, 50, 300);
        cairo_show_text(cr, "Sitio web");
        cairo_tag_end(cr, CAIRO_TAG_LINK);
      }
      cairo_show_page(cr);
    }
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
  }
} // namespace

int main() {
  const auto dir = fs::temp_directory_path() / ("okaru-pdf-" + std::to_string(::getpid()));
  fs::create_directories(dir);
  writeSample(dir / "muestra.pdf");

  auto opened = Document::open(dir / "muestra.pdf");
  expect(opened.has_value(), "abre el PDF");
  if (!opened) {
    return EXIT_FAILURE;
  }
  const Document& doc = **opened;
  expect(doc.pageCount() == 3, "tres páginas");
  const PageSize size = doc.pageSize(0);
  expect(size.width == 400.0 && size.height == 600.0, "tamaño en puntos");

  const auto outline = doc.outline();
  expect(outline.size() == 3 && outline[1].title == "Capítulo dos" && outline[1].page == 1, "índice con su página");

  const auto matches = doc.find(1, "mundo", false, false);
  expect(matches.size() == 1, "búsqueda");
  // El texto está en y≈100 desde arriba: la búsqueda ya viene con el origen arriba.
  expect(!matches.empty() && matches[0].y0 > 70 && matches[0].y1 < 110, "búsqueda con origen arriba");
  expect(doc.find(0, "mundo", false, false).empty(), "no está en otra página");
  expect(doc.find(1, "MUNDO", true, false).empty() && doc.find(1, "MUNDO", false, false).size() == 1, "mayúsculas");

  const auto layout = doc.textLayout(1);
  expect(layout.text.find("Hola mundo OKARU") != std::string::npos, "texto de la página");
  expect(!layout.chars.empty() && layout.chars.size() == layout.offsets.size(), "un rectángulo por carácter");

  const auto links = doc.links(0);
  bool internal = false;
  bool external = false;
  for (const auto& link : links) {
    internal = internal || link.page == 2;
    external = external || link.uri == "https://example.org";
  }
  expect(internal && external, "enlaces interno y externo");

  const auto meta = doc.metadata();
  expect(meta.title == "Documento de prueba" && meta.author == "OKARU" && !meta.version.empty(), "metadatos");

  const auto page = doc.render(0, 0.5, 0, false);
  expect(page.width == 200 && page.height == 300 && page.rgba.size() == 200U * 300U * 4U, "render a escala");
  expect(page.rgba[0] == 255 && page.rgba[3] == 255, "papel blanco opaco");
  const auto rotated = doc.render(0, 0.5, 90, false);
  expect(rotated.width == 300 && rotated.height == 200, "render girado");
  const auto night = doc.render(0, 0.5, 0, true);
  expect(night.rgba[0] == 0, "modo nocturno invierte");

  expect(!Document::open(dir / "no-existe.pdf").has_value(), "un archivo que no existe da error");

  expect(parsePageRange("1-3, 5", 10) == std::vector<int>{0, 1, 2, 4}, "rango de páginas");
  expect(parsePageRange("8-", 10) == std::vector<int>{7, 8, 9}, "rango hasta el final");
  expect(parsePageRange("3,3,1", 10) == std::vector<int>{0, 2}, "sin repetir y ordenado");
  expect(
      parsePageRange("0-2", 10).empty() && parsePageRange("a", 10).empty() && parsePageRange("5-11", 10).empty(),
      "rangos inválidos"
  );

  fs::remove_all(dir);
  return g_ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
