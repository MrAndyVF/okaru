#pragma once

// OKARU: lo que okaru-edit entiende de cada formato además de colorearlo: validar (JSON, JSONC, TOML, XML) marcando
// dónde está el error, dar formato (JSON, JSONC sin comentarios, XML) y leer CSV para verlo como tabla.

#include "apps/editor/syntax.h"

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace okaru::editor {

  struct Problem {
    std::size_t line = 0;   // desde 0
    std::size_t column = 0; // byte, desde 0
    std::string message;    // tal como lo da el analizador (en inglés)
  };

  [[nodiscard]] bool canValidate(Language language);
  [[nodiscard]] bool canFormat(Language language);

  // nullopt = válido (o un formato que no se valida).
  [[nodiscard]] std::optional<Problem> validate(Language language, std::string_view text);

  // `indent` espacios por nivel, o un tabulador si `tabs`.
  [[nodiscard]] std::expected<std::string, Problem>
  format(Language language, std::string_view text, int indent, bool tabs);

  // JSONC → JSON con las mismas posiciones: comentarios y comas finales pasan a espacios (los saltos se quedan).
  [[nodiscard]] std::string stripJsonc(std::string_view text, bool* hadComments = nullptr);

  // CSV (RFC 4180) con el separador más probable de la primera línea: , ; o tabulador.
  [[nodiscard]] std::vector<std::vector<std::string>> parseCsv(std::string_view text);

} // namespace okaru::editor
