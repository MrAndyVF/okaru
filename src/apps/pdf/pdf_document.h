#pragma once

// OKARU: documento PDF de okaru-pdf sobre poppler-glib (el motor de Papers). Coordenadas en puntos con origen arriba a
// la izquierda (poppler da algunas desde abajo: aquí ya vienen corregidas). poppler no es seguro entre hilos con un
// mismo documento: cada hilo abre el suyo.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct _PopplerDocument;

namespace okaru::pdf {

  struct PageSize {
    double width = 0.0; // puntos
    double height = 0.0;
  };

  struct Rect {
    double x0 = 0.0;
    double y0 = 0.0;
    double x1 = 0.0;
    double y1 = 0.0;

    [[nodiscard]] bool contains(double x, double y) const { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
  };

  struct OutlineItem {
    std::string title;
    int page = -1; // desde 0; -1 si no lleva a una página
    int depth = 0;
  };

  struct Link {
    Rect area;
    int page = -1;   // enlace interno
    std::string uri; // enlace externo
  };

  // Texto de una página con el rectángulo de cada carácter (en el orden de `text`, uno por carácter UTF-8).
  struct TextLayout {
    std::string text;
    std::vector<Rect> chars;
    std::vector<std::size_t> offsets; // byte donde empieza cada carácter en `text`
  };

  struct Metadata {
    std::string title;
    std::string author;
    std::string subject;
    std::string keywords;
    std::string creator;
    std::string producer;
    std::string created;
    std::string modified;
    std::string version;
  };

  struct RenderedPage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba; // RGBA8, opaco
  };

  enum class OpenError : std::uint8_t { NeedsPassword, WrongPassword, Damaged };

  // Campo de un formulario (AcroForm) en una página.
  struct FormField {
    enum class Kind : std::uint8_t { Text, Check, Radio, Choice, Other };
    int id = -1;
    Kind kind = Kind::Other;
    Rect area;
    std::string text;       // texto (Text) o etiqueta
    bool checked = false;   // Check/Radio
    bool multiline = false; // Text
    bool password = false;  // Text
    bool readOnly = false;
    int maxLength = -1;               // Text; -1 = sin tope
    std::vector<std::string> choices; // Choice
    int selected = -1;                // Choice
  };

  // Un cambio al documento; se aplica igual al de la ventana y al del hilo que dibuja.
  struct Edit {
    enum class Kind : std::uint8_t { Text, Check, Choice, Highlight, Underline };
    Kind kind = Kind::Text;
    int page = 0;
    int field = -1;          // Text/Check/Choice
    std::string text;        // Text
    bool checked = false;    // Check
    int choice = -1;         // Choice
    std::vector<Rect> rects; // Highlight/Underline: rectángulos de línea (origen arriba)
  };

  class Document {
  public:
    ~Document();
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;

    [[nodiscard]] static std::expected<std::unique_ptr<Document>, OpenError>
    open(const std::filesystem::path& path, const std::string& password = {});

    [[nodiscard]] int pageCount() const;
    [[nodiscard]] PageSize pageSize(int page) const;
    // Número impreso en la página ("iv", "12"); vacío si el PDF no define etiquetas.
    [[nodiscard]] std::string pageLabel(int page) const;
    [[nodiscard]] std::vector<OutlineItem> outline() const;
    [[nodiscard]] std::vector<Link> links(int page) const;
    [[nodiscard]] std::vector<Rect> find(int page, std::string_view text, bool caseSensitive, bool wholeWords) const;
    [[nodiscard]] TextLayout textLayout(int page) const;
    [[nodiscard]] Metadata metadata() const;
    [[nodiscard]] std::vector<FormField> formFields(int page) const;
    bool apply(const Edit& edit);
    // Escribe el documento con sus cambios (a un temporal junto al destino, que luego lo reemplaza).
    [[nodiscard]] bool save(const std::filesystem::path& path, std::string& error) const;

    // `scale` = píxeles por punto; `rotation` 0/90/180/270; `invert` = modo nocturno (colores invertidos).
    [[nodiscard]] RenderedPage render(int page, double scale, int rotation, bool invert) const;

  private:
    explicit Document(_PopplerDocument* doc);

    _PopplerDocument* m_doc = nullptr;
  };

  // Rango "1-3,5,8-" (desde 1) → páginas desde 0, ordenadas y sin repetir; vacío si el texto no es válido.
  [[nodiscard]] std::vector<int> parsePageRange(std::string_view text, int pageCount);

} // namespace okaru::pdf
