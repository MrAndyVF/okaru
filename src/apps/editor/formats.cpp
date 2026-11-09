#include "apps/editor/formats.h"

#include <algorithm>
#include <libxml/parser.h>
#include <libxml/xmlerror.h>
#include <libxml/xmlsave.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <toml++/toml.hpp>

namespace okaru::editor {

  namespace {

    Problem problemAtByte(std::string_view text, std::size_t byte, std::string message) {
      byte = std::min(byte, text.size());
      const std::string_view before = text.substr(0, byte);
      const auto line = static_cast<std::size_t>(std::ranges::count(before, '\n'));
      const auto lastNewline = before.rfind('\n');
      const std::size_t column = lastNewline == std::string_view::npos ? byte : byte - lastNewline - 1;
      return {.line = line, .column = column, .message = std::move(message)};
    }

    std::optional<Problem> validateJson(std::string_view text) {
      try {
        (void)nlohmann::ordered_json::parse(text);
      } catch (const nlohmann::json::parse_error& e) {
        // `byte` es el último carácter leído (desde 1).
        std::string message = e.what();
        if (const auto at = message.find("]: "); at != std::string::npos) {
          message.erase(0, at + 3); // quita "[json.exception.parse_error.101] "
        }
        return problemAtByte(text, e.byte > 0 ? e.byte - 1 : 0, std::move(message));
      }
      return std::nullopt;
    }

    // libxml2 guarda el último error del hilo; se lee justo después de analizar.
    Problem lastXmlProblem() {
      const xmlError* error = xmlGetLastError();
      if (error == nullptr) {
        return {.message = "invalid XML"};
      }
      std::string message = error->message != nullptr ? error->message : "invalid XML";
      while (!message.empty() && (message.back() == '\n' || message.back() == ' ')) {
        message.pop_back();
      }
      return {
          .line = error->line > 0 ? static_cast<std::size_t>(error->line - 1) : 0,
          .column = error->int2 > 0 ? static_cast<std::size_t>(error->int2 - 1) : 0,
          .message = std::move(message),
      };
    }

    constexpr int kXmlOptions = XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING;

    std::string indentUnit(int indent, bool tabs) {
      return tabs ? std::string("\t") : std::string(static_cast<std::size_t>(std::max(1, indent)), ' ');
    }

  } // namespace

  bool canValidate(Language language) {
    return language == Language::Json
        || language == Language::Jsonc
        || language == Language::Toml
        || language == Language::Xml;
  }

  bool canFormat(Language language) {
    return language == Language::Json || language == Language::Jsonc || language == Language::Xml;
  }

  std::string stripJsonc(std::string_view text, bool* hadComments) {
    std::string out(text);
    bool comments = false;
    std::size_t i = 0;
    const auto blank = [&](std::size_t from, std::size_t to) {
      for (std::size_t k = from; k < to && k < out.size(); ++k) {
        if (out[k] != '\n') {
          out[k] = ' ';
        }
      }
    };
    while (i < out.size()) {
      const char c = out[i];
      if (c == '"') {
        for (++i; i < out.size() && out[i] != '"'; ++i) {
          if (out[i] == '\\') {
            ++i;
          }
        }
        ++i;
      } else if (c == '/' && i + 1 < out.size() && out[i + 1] == '/') {
        const auto end = out.find('\n', i);
        blank(i, end == std::string::npos ? out.size() : end);
        comments = true;
        i = end == std::string::npos ? out.size() : end;
      } else if (c == '/' && i + 1 < out.size() && out[i + 1] == '*') {
        const auto end = out.find("*/", i + 2);
        const std::size_t stop = end == std::string::npos ? out.size() : end + 2;
        blank(i, stop);
        comments = true;
        i = stop;
      } else {
        ++i;
      }
    }
    // Coma final antes de } o ] (solo con espacios entre medio, ya sin comentarios).
    for (std::size_t k = 0; k < out.size(); ++k) {
      if (out[k] == '"') {
        for (++k; k < out.size() && out[k] != '"'; ++k) {
          if (out[k] == '\\') {
            ++k;
          }
        }
      } else if (out[k] == ',') {
        std::size_t next = k + 1;
        while (next < out.size() && (out[next] == ' ' || out[next] == '\t' || out[next] == '\n' || out[next] == '\r')) {
          ++next;
        }
        if (next < out.size() && (out[next] == '}' || out[next] == ']')) {
          out[k] = ' ';
        }
      }
    }
    if (hadComments != nullptr) {
      *hadComments = comments;
    }
    return out;
  }

  std::optional<Problem> validate(Language language, std::string_view text) {
    switch (language) {
    case Language::Json:
      return validateJson(text);
    case Language::Jsonc:
      return validateJson(stripJsonc(text));
    case Language::Toml:
      try {
        (void)toml::parse(text);
      } catch (const toml::parse_error& e) {
        const auto& at = e.source().begin;
        return Problem{
            .line = at.line > 0 ? at.line - 1 : 0,
            .column = at.column > 0 ? at.column - 1 : 0,
            .message = std::string(e.description()),
        };
      }
      return std::nullopt;
    case Language::Xml: {
      xmlResetLastError();
      xmlDocPtr doc = xmlReadMemory(text.data(), static_cast<int>(text.size()), "document.xml", nullptr, kXmlOptions);
      if (doc == nullptr) {
        return lastXmlProblem();
      }
      xmlFreeDoc(doc);
      return std::nullopt;
    }
    default:
      return std::nullopt;
    }
  }

  std::expected<std::string, Problem> format(Language language, std::string_view text, int indent, bool tabs) {
    if (language == Language::Json || language == Language::Jsonc) {
      bool comments = false;
      const std::string source = language == Language::Jsonc ? stripJsonc(text, &comments) : std::string(text);
      if (comments) {
        return std::unexpected(Problem{.message = "comments"}); // la ventana lo explica: se perderían
      }
      if (auto problem = validateJson(source)) {
        return std::unexpected(*problem);
      }
      // ordered_json: las claves quedan en el orden del archivo.
      const auto value = nlohmann::ordered_json::parse(source);
      return value.dump(tabs ? 1 : std::max(1, indent), tabs ? '\t' : ' ') + "\n";
    }
    if (language == Language::Xml) {
      xmlResetLastError();
      xmlDocPtr doc = xmlReadMemory(
          text.data(), static_cast<int>(text.size()), "document.xml", nullptr, kXmlOptions | XML_PARSE_NOBLANKS
      );
      if (doc == nullptr) {
        return std::unexpected(lastXmlProblem());
      }
      // Sin la declaración <?xml …?> si el archivo no la tenía.
      const bool declaration = std::string_view(text).find("<?xml") != std::string_view::npos;
      xmlBufferPtr buffer = xmlBufferCreate();
      xmlSaveCtxtPtr save = xmlSaveToBuffer(buffer, "UTF-8", XML_SAVE_FORMAT | (declaration ? 0 : XML_SAVE_NO_DECL));
      xmlSaveDoc(save, doc);
      xmlSaveClose(save);
      std::string formatted(
          reinterpret_cast<const char*>(xmlBufferContent(buffer)), static_cast<std::size_t>(xmlBufferLength(buffer))
      );
      xmlBufferFree(buffer);
      xmlFreeDoc(doc);
      // libxml2 sangra con dos espacios: se cambia cada par del comienzo por la sangría pedida.
      const std::string unit = indentUnit(indent, tabs);
      std::istringstream lines(formatted);
      std::string out;
      for (std::string line; std::getline(lines, line);) {
        std::size_t spaces = 0;
        while (spaces < line.size() && line[spaces] == ' ') {
          ++spaces;
        }
        std::string prefix;
        for (std::size_t level = 0; level < spaces / 2; ++level) {
          prefix += unit;
        }
        out += prefix + line.substr(spaces / 2 * 2) + "\n";
      }
      return out;
    }
    return std::unexpected(Problem{.message = "unsupported"});
  }

  std::vector<std::vector<std::string>> parseCsv(std::string_view text) {
    const std::string_view firstLine = text.substr(0, text.find('\n'));
    char delimiter = ',';
    std::size_t best = 0;
    for (const char candidate : {',', ';', '\t'}) {
      const auto count = static_cast<std::size_t>(std::ranges::count(firstLine, candidate));
      if (count > best) {
        best = count;
        delimiter = candidate;
      }
    }
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string field;
    bool quoted = false;
    bool any = false; // la fila tiene algo (para no agregar una vacía al final)
    for (std::size_t i = 0; i < text.size(); ++i) {
      const char c = text[i];
      if (quoted) {
        if (c == '"' && i + 1 < text.size() && text[i + 1] == '"') {
          field += '"';
          ++i;
        } else if (c == '"') {
          quoted = false;
        } else {
          field += c;
        }
      } else if (c == '"') {
        quoted = true;
        any = true;
      } else if (c == delimiter) {
        row.push_back(std::move(field));
        field.clear();
        any = true;
      } else if (c == '\n' || c == '\r') {
        if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
          ++i;
        }
        if (any || !field.empty()) {
          row.push_back(std::move(field));
          rows.push_back(std::move(row));
        }
        row.clear();
        field.clear();
        any = false;
      } else {
        field += c;
        any = true;
      }
    }
    if (any || !field.empty()) {
      row.push_back(std::move(field));
      rows.push_back(std::move(row));
    }
    return rows;
  }

} // namespace okaru::editor
