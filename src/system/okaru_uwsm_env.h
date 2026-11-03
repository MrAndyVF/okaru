#pragma once

// OKARU: variables de entorno de la sesión en ~/.config/uwsm/env (`export CLAVE=valor`). Se editan en su
// lugar: comentarios, orden y líneas que no son export se conservan.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace okaru::uwsm {

  struct EnvVar {
    std::string key;
    std::string value;    // sin comillas
    std::string comment;  // comentarios justo encima, sin "# "
    bool managed = false; // la reescribe otra herramienta (cursor-plugin-sync): solo lectura
  };

  [[nodiscard]] std::vector<EnvVar> parseEnv(std::string_view text);
  [[nodiscard]] bool isValidKey(std::string_view key);
  // Cambia el valor en su lugar, o agrega `export CLAVE=valor` al final.
  [[nodiscard]] std::string setVar(std::string_view text, std::string_view key, std::string_view value);
  [[nodiscard]] std::string removeVar(std::string_view text, std::string_view key);
  // Comillas simples si hace falta: `a b` -> `'a b'`.
  [[nodiscard]] std::string quote(std::string_view value);

  [[nodiscard]] std::filesystem::path envFile();
  [[nodiscard]] std::string readEnvText();
  [[nodiscard]] std::optional<std::string> writeEnvText(const std::string& text);

} // namespace okaru::uwsm
