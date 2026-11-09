#pragma once

// OKARU: resaltado de okaru-edit. Un analizador pequeño por formato que trabaja línea a línea: recibe el estado con
// el que terminó la línea anterior (comentario o cadena abiertos…) y devuelve el suyo, así al editar solo se vuelve a
// analizar desde la línea cambiada hasta que el estado coincide con el de antes.

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

namespace okaru::editor {

  enum class Language : std::uint8_t { Text, Html, Xml, Json, Jsonc, Toml, Css, Conf, Lua, Kdl, Csv };
  inline constexpr std::size_t kLanguageCount = 11;

  enum class Token : std::uint8_t {
    Plain,
    Keyword,  // palabras reservadas, at-rules de CSS
    String,   // cadenas y valores de atributos
    Number,   // números, fechas de TOML, colores #hex
    Constant, // true/false/null/nil
    Comment,
    Tag,         // etiquetas de HTML/XML, nombres de nodo de KDL, selectores de CSS
    Attribute,   // atributos de HTML/XML, propiedades de KDL
    Key,         // claves de JSON/TOML/conf, propiedades de CSS
    Section,     // [tabla] de TOML, [sección] de conf
    Function,    // nombres de función de Lua, funciones de CSS
    Punctuation, // llaves, corchetes, =, :, entidades &amp;
    Column0,     // CSV: una columna de cada color, en ciclo
    Column1,
    Column2,
    Column3,
    Column4,
    Column5,
  };

  struct Span {
    std::uint32_t start = 0; // byte
    std::uint32_t length = 0;
    Token token = Token::Plain;

    bool operator==(const Span&) const = default;
  };

  using LineState = std::uint32_t;

  // Analiza una línea; `out` recibe los tramos coloreados (lo que no aparece es Plain). Devuelve el estado de salida.
  LineState highlightLine(Language language, std::string_view line, LineState in, std::vector<Span>& out);

  // Por extensión o nombre (config.kdl, .bashrc → conf…); si no se reconoce, por la primera línea (<?xml, <!DOCTYPE).
  [[nodiscard]] Language detectLanguage(const std::filesystem::path& path, std::string_view firstLine = {});
  // "html", "json"… (claves i18n apps.edit.language.*).
  [[nodiscard]] std::string_view languageId(Language language);
  // Prefijo de comentario de línea ("//", "#", "--"); vacío si el formato no tiene.
  [[nodiscard]] std::string_view lineComment(Language language);

} // namespace okaru::editor
