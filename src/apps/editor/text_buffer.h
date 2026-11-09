#pragma once

// OKARU: texto de okaru-edit. Líneas en UTF-8 (sin el salto), posiciones en bytes dentro de la línea, deshacer y
// rehacer por grupos (escribir una palabra es un solo paso) y búsqueda. Sin UI: lo prueba okaru_editor_test.

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace okaru::editor {

  struct TextPos {
    std::size_t line = 0;
    std::size_t col = 0; // byte dentro de la línea

    auto operator<=>(const TextPos&) const = default;
  };

  struct TextRange {
    TextPos from;
    TextPos to;

    [[nodiscard]] bool empty() const { return from == to; }
    [[nodiscard]] TextRange ordered() const { return from <= to ? *this : TextRange{to, from}; }
    bool operator==(const TextRange&) const = default;
  };

  struct SearchOptions {
    bool caseSensitive = false;
    bool wholeWord = false;
  };

  class TextBuffer {
  public:
    explicit TextBuffer(std::string_view text = {});

    // Reemplaza todo y olvida el historial (al abrir o recargar un archivo).
    void setText(std::string_view text);
    // Con el salto de línea original del archivo (LF o CRLF) y el salto final si lo tenía.
    [[nodiscard]] std::string text() const;
    [[nodiscard]] const std::vector<std::string>& lines() const noexcept { return m_lines; }
    [[nodiscard]] std::size_t lineCount() const noexcept { return m_lines.size(); }
    [[nodiscard]] const std::string& line(std::size_t index) const { return m_lines[index]; }
    [[nodiscard]] bool crlf() const noexcept { return m_crlf; }
    void setCrlf(bool crlf);

    [[nodiscard]] TextPos clamp(TextPos pos) const;
    [[nodiscard]] TextPos end() const { return {m_lines.size() - 1, m_lines.back().size()}; }
    [[nodiscard]] std::string textIn(TextRange range) const;

    // Edición; cada una queda en el historial. Devuelven dónde termina el texto insertado.
    TextPos insert(TextPos at, std::string_view text);
    void erase(TextRange range);
    TextPos replace(TextRange range, std::string_view text);

    // Movimiento por caracteres UTF-8 y por palabras.
    [[nodiscard]] TextPos nextChar(TextPos pos) const;
    [[nodiscard]] TextPos prevChar(TextPos pos) const;
    [[nodiscard]] TextPos nextWord(TextPos pos) const;
    [[nodiscard]] TextPos prevWord(TextPos pos) const;
    [[nodiscard]] TextRange wordAt(TextPos pos) const;

    // Deshacer/rehacer devuelven dónde dejar el cursor. Escribir seguido se junta en un paso hasta que el cursor
    // se mueve, se borra o se escribe un espacio o un salto (breakUndoGroup lo fuerza).
    std::optional<TextPos> undo();
    std::optional<TextPos> redo();
    void breakUndoGroup() noexcept { m_mergeable = false; }
    [[nodiscard]] bool canUndo() const noexcept { return m_history > 0; }
    [[nodiscard]] bool canRedo() const noexcept { return m_history < m_edits.size(); }

    // Cambios desde la última vez que se guardó (deshacer hasta ese punto lo deja sin cambios).
    [[nodiscard]] bool modified() const noexcept { return m_history != m_savedAt; }
    void markSaved() noexcept;
    // Sube con cada cambio; sirve para saber si algo hay que volver a calcular.
    [[nodiscard]] std::uint64_t version() const noexcept { return m_version; }
    // Primera línea cambiada desde la última llamada (para volver a resaltar desde ahí).
    [[nodiscard]] std::size_t takeDirtyFrom();

    [[nodiscard]] std::optional<TextRange>
    find(std::string_view needle, TextPos from, bool forward, const SearchOptions& options) const;
    [[nodiscard]] std::vector<TextRange> findAll(std::string_view needle, const SearchOptions& options) const;
    // Un solo paso de deshacer.
    std::size_t replaceAll(std::string_view needle, std::string_view replacement, const SearchOptions& options);

  private:
    struct Edit {
      TextPos at;
      std::string removed;
      std::string inserted;
    };

    TextPos rawInsert(TextPos at, std::string_view text);
    void rawErase(TextRange range);
    void record(Edit edit, bool mergeable);
    void touched(std::size_t line);

    std::vector<std::string> m_lines{std::string()};
    bool m_crlf = false;
    std::vector<Edit> m_edits;
    std::vector<std::size_t> m_groupEnds; // m_edits.size() al cerrar cada grupo
    std::size_t m_history = 0;            // grupos aplicados
    std::size_t m_savedAt = 0;
    bool m_mergeable = false;
    std::uint64_t m_version = 0;
    std::size_t m_dirtyFrom = 0;
  };

  // Ancho en celdas de un carácter (2 los de Asia oriental y emoji; 0 los combinantes).
  [[nodiscard]] int cellWidth(char32_t codepoint);
  // Siguiente carácter UTF-8 desde `at`; avanza `at`.
  [[nodiscard]] char32_t decodeUtf8(std::string_view text, std::size_t& at);

} // namespace okaru::editor
