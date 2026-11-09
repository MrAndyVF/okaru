// OKARU: núcleo de okaru-edit (texto, deshacer, búsqueda, resaltado, validación, formato y CSV).
#include "apps/editor/formats.h"
#include "apps/editor/syntax.h"
#include "apps/editor/text_buffer.h"

#include <clocale>
#include <cstdlib>
#include <print>

using namespace okaru::editor;

namespace {
  bool g_ok = true;

  void expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "okaru_editor_test: {}", message);
      g_ok = false;
    }
  }

  // Texto del primer tramo del tipo pedido, para comprobar el resaltado sin depender de posiciones exactas.
  std::string
  first(Language language, std::string_view line, Token token, LineState in = 0, LineState* outState = nullptr) {
    std::vector<Span> spans;
    const LineState state = highlightLine(language, line, in, spans);
    if (outState != nullptr) {
      *outState = state;
    }
    for (const auto& span : spans) {
      if (span.token == token) {
        return std::string(line.substr(span.start, span.length));
      }
    }
    return {};
  }
} // namespace

int main() {
  std::setlocale(LC_ALL, "C.UTF-8");

  // ── Texto ──────────────────────────────────────────────────────────────────────────────────────────
  TextBuffer buffer("hola\r\nmundo\r\n");
  expect(buffer.lineCount() == 3 && buffer.crlf() && buffer.line(1) == "mundo", "CRLF y salto final");
  expect(buffer.text() == "hola\r\nmundo\r\n", "se guarda igual que se abrió");
  expect(!buffer.modified(), "recién abierto no está modificado");

  // Escribir seguido es un solo paso de deshacer.
  TextPos cursor{0, 4};
  for (const char* c : {",", "x", "y"}) {
    cursor = buffer.insert(cursor, c);
  }
  expect(buffer.line(0) == "hola,xy" && buffer.modified(), "escribir");
  expect(buffer.undo() == TextPos{0, 4} && buffer.line(0) == "hola" && !buffer.modified(), "deshacer junta lo escrito");
  expect(buffer.redo().has_value() && buffer.line(0) == "hola,xy", "rehacer");
  (void)buffer.undo();
  buffer.markSaved();

  const TextPos afterPaste = buffer.insert({1, 5}, "\nuno\ndos");
  expect(afterPaste == TextPos{3, 3} && buffer.lineCount() == 5 && buffer.line(3) == "dos", "pegar varias líneas");
  buffer.erase({{1, 2}, {3, 1}});
  expect(buffer.line(1) == "muos" && buffer.lineCount() == 3, "borrar entre líneas");
  (void)buffer.undo();
  expect(buffer.line(3) == "dos" && buffer.lineCount() == 5, "deshacer el borrado");

  // Movimiento UTF-8 y por palabras.
  TextBuffer words("año  nuevo_2026, ¿sí?");
  expect(words.nextChar({0, 1}) == TextPos{0, 3}, "ñ ocupa dos bytes");
  expect(words.prevChar({0, 3}) == TextPos{0, 1}, "volver sobre la ñ");
  expect(words.nextWord({0, 0}) == TextPos{0, 4}, "Ctrl+→ salta la palabra");
  expect(words.prevWord({0, 15}) == TextPos{0, 6}, "Ctrl+← vuelve al inicio de la palabra");
  expect(words.textIn(words.wordAt({0, 8})) == "nuevo_2026", "doble clic toma la palabra");
  expect(cellWidth(U'a') == 1 && cellWidth(U'漢') == 2, "ancho de celdas");

  // Búsqueda y reemplazo.
  TextBuffer search("Gato gato\ngatos GATO");
  expect(search.findAll("gato", {}).size() == 4, "sin distinguir mayúsculas");
  expect(search.findAll("gato", {.caseSensitive = true}).size() == 2, "distinguiendo mayúsculas");
  expect(search.findAll("gato", {.wholeWord = true}).size() == 3, "palabra completa");
  const auto back = search.find("gato", {1, 0}, false, {.caseSensitive = true});
  expect(back.has_value() && back->from == TextPos{0, 5}, "buscar hacia atrás");
  expect(search.replaceAll("gato", "perro", {.wholeWord = true}) == 3, "reemplazar todo");
  expect(search.text() == "perro perro\ngatos perro", "texto reemplazado");
  (void)search.undo();
  expect(search.text() == "Gato gato\ngatos GATO", "reemplazar todo se deshace en un paso");

  // ── Resaltado ──────────────────────────────────────────────────────────────────────────────────────
  expect(
      first(Language::Json, R"(  "clave": "valor", "n": -1.5e3, "b": true)", Token::Key) == "\"clave\"", "JSON clave"
  );
  expect(first(Language::Json, R"("a": "valor")", Token::String) == "\"valor\"", "JSON cadena");
  expect(first(Language::Json, R"("n": -1.5e3)", Token::Number) == "-1.5e3", "JSON número");
  LineState state = 0;
  expect(
      first(Language::Jsonc, "1, /* abre", Token::Comment, 0, &state) == "/* abre" && state == 1,
      "JSONC comentario abierto"
  );
  expect(
      first(Language::Jsonc, "cierra */ 2", Token::Comment, state, &state) == "cierra */" && state == 0,
      "JSONC lo cierra"
  );

  expect(first(Language::Html, R"(<a href="x">texto</a>)", Token::Tag) == "a", "HTML etiqueta");
  expect(first(Language::Html, R"(<a href="x">)", Token::Attribute) == "href", "HTML atributo");
  expect(first(Language::Html, R"(<a href="x">)", Token::String) == "\"x\"", "HTML valor");
  expect(
      first(Language::Xml, "<!-- uno", Token::Comment, 0, &state) == "<!-- uno" && state != 0, "XML comentario abierto"
  );
  expect(
      first(Language::Xml, "dos --> <b/>", Token::Tag, state, &state) == "b" && state == 0, "XML después del comentario"
  );

  expect(first(Language::Toml, "[servidor.web]", Token::Section) == "[servidor.web]", "TOML tabla");
  expect(first(Language::Toml, R"(puerto = 8080 # comentario)", Token::Key) == "puerto", "TOML clave");
  expect(first(Language::Toml, R"(puerto = 8080 # comentario)", Token::Number) == "8080", "TOML número");
  expect(first(Language::Toml, R"(t = 1979-05-27T07:32:00Z)", Token::Number) == "1979-05-27T07:32:00Z", "TOML fecha");
  expect(
      first(Language::Toml, R"(texto = """abre)", Token::String, 0, &state) == R"("""abre)" && state == 1,
      "TOML cadena multilínea"
  );

  expect(first(Language::Css, "a:hover, .b {", Token::Tag) == "a:hover", "CSS selector");
  expect(first(Language::Css, "  color: #ff0000;", Token::Key, 2) == "color", "CSS propiedad");
  expect(first(Language::Css, "  color: #ff0000;", Token::Number, 2) == "#ff0000", "CSS color");
  expect(first(Language::Css, "@media (max-width: 600px) {", Token::Keyword) == "@media", "CSS at-rule");

  expect(first(Language::Conf, "[General]", Token::Section) == "[General]", "conf sección");
  expect(first(Language::Conf, "Nombre = valor ; nota", Token::Key) == "Nombre", "conf clave");
  expect(first(Language::Conf, "activo=yes", Token::Constant) == "yes", "conf constante");
  expect(first(Language::Conf, "# comentario", Token::Comment) == "# comentario", "conf comentario");

  expect(
      first(Language::Lua, "local function hola(x) return nil end", Token::Keyword) == "local", "Lua palabra reservada"
  );
  expect(first(Language::Lua, "local function hola(x)", Token::Function) == "hola", "Lua función");
  expect(
      first(Language::Lua, "x = [==[ abre", Token::String, 0, &state) == "[==[ abre" && state != 0, "Lua cadena larga"
  );
  expect(
      first(Language::Lua, "]=] sigue ]==] fin", Token::String, state, &state) == "]=] sigue ]==]" && state == 0,
      "Lua la cierra con el mismo nivel"
  );

  expect(first(Language::Kdl, R"(output "DP-1" { mode "1920x1080" })", Token::Tag) == "output", "KDL nodo");
  expect(first(Language::Kdl, R"(node key=1 #true)", Token::Attribute) == "key", "KDL propiedad");
  expect(first(Language::Kdl, R"(node key=1 #true)", Token::Constant) == "#true", "KDL constante");
  expect(
      first(Language::Kdl, "/* a /* b */", Token::Comment, 0, &state) == "/* a /* b */" && state == 1,
      "KDL comentarios anidados"
  );

  std::vector<Span> spans;
  (void)highlightLine(Language::Csv, R"(uno,"dos, tres",cuatro)", 0, spans);
  expect(spans.size() == 5 && spans[2].token == Token::Column1 && spans[2].length == 11, "CSV columnas y comillas");

  expect(
      detectLanguage("index.HTML") == Language::Html && detectLanguage("a.kdl") == Language::Kdl,
      "detectar por extensión"
  );
  expect(
      detectLanguage("settings.json") == Language::Jsonc && detectLanguage(".bashrc") == Language::Conf,
      "detectar por nombre"
  );
  expect(detectLanguage("sin-extension", "<?xml version=\"1.0\"?>") == Language::Xml, "detectar por contenido");
  expect(lineComment(Language::Lua) == "--" && lineComment(Language::Json).empty(), "comentario de línea");

  // ── Validación, formato y CSV ──────────────────────────────────────────────────────────────────────
  expect(!validate(Language::Json, R"({"a": [1, 2]})").has_value(), "JSON válido");
  const auto jsonProblem = validate(Language::Json, "{\n  \"a\": 1,\n  \"b\" 2\n}");
  expect(jsonProblem.has_value() && jsonProblem->line == 2, "JSON inválido marca la línea");
  expect(
      !validate(Language::Jsonc, "{\n  // nota\n  \"a\": 1, /* x */\n}").has_value(),
      "JSONC con comentarios y coma final"
  );
  const auto tomlProblem = validate(Language::Toml, "a = 1\nb = \n");
  expect(tomlProblem.has_value() && tomlProblem->line == 1, "TOML inválido marca la línea");
  const auto xmlProblem = validate(Language::Xml, "<a>\n  <b>\n</a>\n");
  expect(xmlProblem.has_value() && xmlProblem->line == 2, "XML mal cerrado");

  const auto formatted = format(Language::Json, R"({"z":1,"a":[1,2]})", 2, false);
  expect(
      formatted && *formatted == "{\n  \"z\": 1,\n  \"a\": [\n    1,\n    2\n  ]\n}\n", "formato JSON conserva el orden"
  );
  expect(!format(Language::Jsonc, "{ // nota\n}", 2, false).has_value(), "JSONC con comentarios no se formatea");
  const auto xml = format(Language::Xml, "<a><b>x</b></a>", 4, false);
  expect(xml && *xml == "<a>\n    <b>x</b>\n</a>\n", "formato XML con la sangría pedida y sin declaración nueva");

  const auto rows = parseCsv("nombre;edad\n\"Pérez; Ana\";30\n\"dice \"\"hola\"\"\";5\n");
  expect(rows.size() == 3 && rows[1][0] == "Pérez; Ana" && rows[2][0] == "dice \"hola\"", "CSV con ; y comillas");

  return g_ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
