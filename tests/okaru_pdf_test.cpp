// OKARU: núcleo de okaru-pdf. El PDF de prueba se genera con cairo: tres páginas, texto, un enlace interno, uno
// externo y un índice.
#include "apps/pdf/pdf_document.h"

#include <cairo-pdf.h>
#include <cairo.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <unistd.h>
#include <vector>

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

  // Formulario mínimo a mano: un campo de texto, una casilla y una línea de texto para resaltar. Las posiciones del
  // xref se calculan al escribir.
  void writeForm(const fs::path& path) {
    const std::string content = "BT /F1 18 Tf 40 700 Td (Texto para resaltar) Tj ET";
    const std::vector<std::string> objects = {
        "<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [4 0 R 5 0 R] /NeedAppearances true "
        "/DA (/Helv 12 Tf 0 g) /DR << /Font << /Helv 7 0 R >> >> >> >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 6 0 R /Annots [4 0 R 5 0 R] "
        "/Resources << /Font << /F1 7 0 R >> >> >>",
        "<< /Type /Annot /Subtype /Widget /FT /Tx /T (nombre) /V (viejo) /Rect [40 600 300 625] /P 3 0 R "
        "/DA (/Helv 12 Tf 0 g) /F 4 >>",
        "<< /Type /Annot /Subtype /Widget /FT /Btn /T (acepto) /V /Off /AS /Off /Rect [40 550 60 570] /P 3 0 R /F 4 "
        "/AP << /N << /Yes 8 0 R /Off 8 0 R >> >> >>",
        "<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "\nendstream",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        "<< /Type /XObject /Subtype /Form /BBox [0 0 20 20] /Length 0 >>\nstream\n\nendstream",
    };
    std::string pdf = "%PDF-1.7\n";
    std::vector<std::size_t> offsets;
    for (std::size_t i = 0; i < objects.size(); ++i) {
      offsets.push_back(pdf.size());
      pdf += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    const std::size_t xref = pdf.size();
    pdf += "xref\n0 " + std::to_string(objects.size() + 1) + "\n0000000000 65535 f \n";
    for (const std::size_t offset : offsets) {
      char line[24];
      std::snprintf(line, sizeof(line), "%010zu 00000 n \n", offset);
      pdf += line;
    }
    pdf += "trailer\n<< /Size "
        + std::to_string(objects.size() + 1)
        + " /Root 1 0 R >>\nstartxref\n"
        + std::to_string(xref)
        + "\n%%EOF\n";
    std::ofstream(path, std::ios::binary) << pdf;
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

  {
    // Formulario: rellenar, marcar, resaltar, guardar y volver a abrir.
    writeForm(dir / "formulario.pdf");
    auto form = Document::open(dir / "formulario.pdf");
    expect(form.has_value(), "abre el formulario");
    if (form) {
      auto& doc = **form;
      const auto fields = doc.formFields(0);
      expect(fields.size() == 2, "dos campos");
      const FormField* text = nullptr;
      const FormField* check = nullptr;
      for (const auto& f : fields) {
        (f.kind == FormField::Kind::Text ? text : check) = &f;
      }
      expect(text != nullptr && text->text == "viejo", "campo de texto con su valor");
      expect(text != nullptr && text->area.y0 > 160.0 && text->area.y1 < 195.0, "área con el origen arriba");
      expect(check != nullptr && check->kind == FormField::Kind::Check && !check->checked, "casilla sin marcar");
      if (text != nullptr && check != nullptr) {
        expect(doc.apply({.kind = Edit::Kind::Text, .field = text->id, .text = "Kevin Andrés"}), "escribe el texto");
        expect(doc.apply({.kind = Edit::Kind::Check, .field = check->id, .checked = true}), "marca la casilla");
        expect(doc.apply({.kind = Edit::Kind::Highlight, .page = 0, .rects = {{40, 75, 250, 95}}}), "resalta");
        std::string error;
        expect(doc.save(dir / "formulario.pdf", error), "guarda encima del abierto");
        auto again = Document::open(dir / "formulario.pdf");
        expect(again.has_value(), "vuelve a abrir lo guardado");
        if (again) {
          bool textOk = false;
          bool checkOk = false;
          for (const auto& f : (*again)->formFields(0)) {
            textOk = textOk || (f.kind == FormField::Kind::Text && f.text == "Kevin Andrés");
            checkOk = checkOk || (f.kind == FormField::Kind::Check && f.checked);
          }
          expect(textOk, "el texto quedó guardado");
          expect(checkOk, "la casilla quedó marcada");
        }
      }
    }
  }

  fs::remove_all(dir);
  return g_ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
