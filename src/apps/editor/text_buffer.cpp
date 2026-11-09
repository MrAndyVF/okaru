#include "apps/editor/text_buffer.h"

#include <algorithm>
#include <cwchar>

namespace okaru::editor {

  namespace {

    bool isContinuation(unsigned char c) { return (c & 0xC0U) == 0x80U; }

    bool isWordChar(char32_t c) {
      return c == U'_' || (c >= U'0' && c <= U'9') || (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z') || c >= 0x80;
    }

    char asciiLower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

    // Dónde termina `text` si se inserta en `at`.
    TextPos endAfter(TextPos at, std::string_view text) {
      const auto newline = text.rfind('\n');
      if (newline == std::string_view::npos) {
        return {at.line, at.col + text.size()};
      }
      return {at.line + static_cast<std::size_t>(std::ranges::count(text, '\n')), text.size() - newline - 1};
    }

  } // namespace

  char32_t decodeUtf8(std::string_view text, std::size_t& at) {
    const auto lead = static_cast<unsigned char>(text[at]);
    const std::size_t length = lead < 0x80 ? 1 : (lead & 0xE0) == 0xC0 ? 2 : (lead & 0xF0) == 0xE0 ? 3 : 4;
    if (length == 1 || at + length > text.size()) {
      ++at;
      return lead;
    }
    char32_t cp = lead & (0xFFU >> (length + 1));
    for (std::size_t i = 1; i < length; ++i) {
      cp = (cp << 6U) | (static_cast<unsigned char>(text[at + i]) & 0x3FU);
    }
    at += length;
    return cp;
  }

  int cellWidth(char32_t codepoint) {
    if (codepoint < 0x80) {
      return 1;
    }
    const int width = ::wcwidth(static_cast<wchar_t>(codepoint));
    return width < 0 ? 1 : width;
  }

  TextBuffer::TextBuffer(std::string_view text) { setText(text); }

  void TextBuffer::setText(std::string_view text) {
    m_lines.clear();
    m_crlf = text.find("\r\n") != std::string_view::npos;
    std::size_t start = 0;
    while (true) {
      const auto newline = text.find('\n', start);
      std::string_view line =
          text.substr(start, newline == std::string_view::npos ? std::string_view::npos : newline - start);
      if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
      }
      m_lines.emplace_back(line);
      if (newline == std::string_view::npos) {
        break;
      }
      start = newline + 1;
    }
    m_edits.clear();
    m_groupEnds.clear();
    m_history = 0;
    m_savedAt = 0;
    m_mergeable = false;
    ++m_version;
    m_dirtyFrom = 0;
  }

  std::string TextBuffer::text() const {
    std::string out;
    const std::string_view newline = m_crlf ? "\r\n" : "\n";
    for (std::size_t i = 0; i < m_lines.size(); ++i) {
      if (i > 0) {
        out += newline;
      }
      out += m_lines[i];
    }
    return out;
  }

  void TextBuffer::setCrlf(bool crlf) {
    if (m_crlf != crlf) {
      m_crlf = crlf;
      ++m_version;
      m_savedAt = static_cast<std::size_t>(-1); // cambia el archivo aunque no el texto
    }
  }

  TextPos TextBuffer::clamp(TextPos pos) const {
    pos.line = std::min(pos.line, m_lines.size() - 1);
    const std::string& line = m_lines[pos.line];
    pos.col = std::min(pos.col, line.size());
    while (pos.col > 0 && pos.col < line.size() && isContinuation(static_cast<unsigned char>(line[pos.col]))) {
      --pos.col;
    }
    return pos;
  }

  std::string TextBuffer::textIn(TextRange range) const {
    const auto [from, to] = range.ordered();
    if (from.line == to.line) {
      return m_lines[from.line].substr(from.col, to.col - from.col);
    }
    std::string out = m_lines[from.line].substr(from.col);
    for (std::size_t i = from.line + 1; i < to.line; ++i) {
      out += '\n';
      out += m_lines[i];
    }
    out += '\n';
    out += m_lines[to.line].substr(0, to.col);
    return out;
  }

  void TextBuffer::touched(std::size_t line) {
    m_dirtyFrom = std::min(m_dirtyFrom, line);
    ++m_version;
  }

  std::size_t TextBuffer::takeDirtyFrom() {
    const std::size_t from = m_dirtyFrom;
    m_dirtyFrom = static_cast<std::size_t>(-1);
    return from;
  }

  TextPos TextBuffer::rawInsert(TextPos at, std::string_view text) {
    at = clamp(at);
    touched(at.line);
    std::string tail = m_lines[at.line].substr(at.col);
    m_lines[at.line].erase(at.col);
    std::size_t line = at.line;
    std::size_t start = 0;
    while (true) {
      const auto newline = text.find('\n', start);
      std::string_view piece =
          text.substr(start, newline == std::string_view::npos ? std::string_view::npos : newline - start);
      if (!piece.empty() && piece.back() == '\r') {
        piece.remove_suffix(1); // texto pegado con CRLF
      }
      m_lines[line] += piece;
      if (newline == std::string_view::npos) {
        break;
      }
      m_lines.insert(m_lines.begin() + static_cast<std::ptrdiff_t>(++line), std::string());
      start = newline + 1;
    }
    const TextPos endPos{line, m_lines[line].size()};
    m_lines[line] += tail;
    return endPos;
  }

  void TextBuffer::rawErase(TextRange range) {
    const auto [from, to] = TextRange{clamp(range.from), clamp(range.to)}.ordered();
    if (from == to) {
      return;
    }
    touched(from.line);
    std::string tail = m_lines[to.line].substr(to.col);
    m_lines[from.line].erase(from.col);
    m_lines[from.line] += tail;
    m_lines.erase(
        m_lines.begin() + static_cast<std::ptrdiff_t>(from.line) + 1,
        m_lines.begin() + static_cast<std::ptrdiff_t>(to.line) + 1
    );
  }

  void TextBuffer::record(Edit edit, bool mergeable) {
    // Una edición nueva descarta lo deshecho.
    const std::size_t keep = m_history == 0 ? 0 : m_groupEnds[m_history - 1];
    m_edits.resize(keep);
    m_groupEnds.resize(m_history);
    if (m_savedAt > m_history) {
      m_savedAt = static_cast<std::size_t>(-1);
    }
    const bool merge = mergeable && m_mergeable && m_history > 0;
    m_edits.push_back(std::move(edit));
    if (merge) {
      m_groupEnds.back() = m_edits.size();
    } else {
      m_groupEnds.push_back(m_edits.size());
      ++m_history;
    }
    m_mergeable = mergeable;
  }

  TextPos TextBuffer::insert(TextPos at, std::string_view text) {
    at = clamp(at);
    if (text.empty()) {
      return at;
    }
    // Escribir carácter a carácter se junta; un espacio, un salto o pegar algo largo empieza otro paso.
    std::size_t index = 0;
    const char32_t first = decodeUtf8(text, index);
    const bool typing = index == text.size() && first != U' ' && first != U'\n' && first != U'\t';
    const bool contiguous = !m_edits.empty()
        && m_edits.back().removed.empty()
        && endAfter(m_edits.back().at, m_edits.back().inserted) == at;
    const TextPos endPos = rawInsert(at, text);
    record({.at = at, .removed = {}, .inserted = std::string(text)}, typing && contiguous);
    if (!typing) {
      m_mergeable = false;
    } else if (!contiguous) {
      m_mergeable = true; // el primer carácter abre un grupo al que se suman los siguientes
    }
    return endPos;
  }

  void TextBuffer::erase(TextRange range) {
    range = TextRange{clamp(range.from), clamp(range.to)}.ordered();
    if (range.empty()) {
      return;
    }
    std::string removed = textIn(range);
    rawErase(range);
    record({.at = range.from, .removed = std::move(removed), .inserted = {}}, false);
  }

  TextPos TextBuffer::replace(TextRange range, std::string_view text) {
    range = TextRange{clamp(range.from), clamp(range.to)}.ordered();
    std::string removed = textIn(range);
    rawErase(range);
    const TextPos endPos = rawInsert(range.from, text);
    record({.at = range.from, .removed = std::move(removed), .inserted = std::string(text)}, false);
    return endPos;
  }

  std::optional<TextPos> TextBuffer::undo() {
    if (m_history == 0) {
      return std::nullopt;
    }
    const std::size_t first = m_history > 1 ? m_groupEnds[m_history - 2] : 0;
    const std::size_t last = m_groupEnds[m_history - 1];
    TextPos cursor;
    for (std::size_t i = last; i-- > first;) {
      const Edit& edit = m_edits[i];
      rawErase({edit.at, endAfter(edit.at, edit.inserted)});
      cursor = rawInsert(edit.at, edit.removed);
    }
    --m_history;
    m_mergeable = false;
    return cursor;
  }

  std::optional<TextPos> TextBuffer::redo() {
    if (m_history >= m_groupEnds.size()) {
      return std::nullopt;
    }
    const std::size_t first = m_history > 0 ? m_groupEnds[m_history - 1] : 0;
    const std::size_t last = m_groupEnds[m_history];
    TextPos cursor;
    for (std::size_t i = first; i < last; ++i) {
      const Edit& edit = m_edits[i];
      rawErase({edit.at, endAfter(edit.at, edit.removed)});
      cursor = rawInsert(edit.at, edit.inserted);
    }
    ++m_history;
    m_mergeable = false;
    return cursor;
  }

  void TextBuffer::markSaved() noexcept {
    m_savedAt = m_history;
    m_mergeable = false;
  }

  TextPos TextBuffer::nextChar(TextPos pos) const {
    pos = clamp(pos);
    const std::string& line = m_lines[pos.line];
    if (pos.col >= line.size()) {
      return pos.line + 1 < m_lines.size() ? TextPos{pos.line + 1, 0} : pos;
    }
    (void)decodeUtf8(line, pos.col);
    return pos;
  }

  TextPos TextBuffer::prevChar(TextPos pos) const {
    pos = clamp(pos);
    if (pos.col == 0) {
      return pos.line > 0 ? TextPos{pos.line - 1, m_lines[pos.line - 1].size()} : pos;
    }
    const std::string& line = m_lines[pos.line];
    do {
      --pos.col;
    } while (pos.col > 0 && isContinuation(static_cast<unsigned char>(line[pos.col])));
    return pos;
  }

  TextPos TextBuffer::nextWord(TextPos pos) const {
    pos = clamp(pos);
    const std::string& line = m_lines[pos.line];
    if (pos.col >= line.size()) {
      return nextChar(pos);
    }
    // Salta lo que no es palabra y luego la palabra, como Ctrl+→ en GNOME.
    std::size_t at = pos.col;
    while (at < line.size()) {
      std::size_t next = at;
      if (isWordChar(decodeUtf8(line, next))) {
        break;
      }
      at = next;
    }
    while (at < line.size()) {
      std::size_t next = at;
      if (!isWordChar(decodeUtf8(line, next))) {
        break;
      }
      at = next;
    }
    return {pos.line, at};
  }

  TextPos TextBuffer::prevWord(TextPos pos) const {
    pos = clamp(pos);
    if (pos.col == 0) {
      return prevChar(pos);
    }
    const auto charBefore = [&](TextPos p) {
      std::size_t at = prevChar(p).col;
      return decodeUtf8(m_lines[p.line], at);
    };
    while (pos.col > 0 && !isWordChar(charBefore(pos))) {
      pos = prevChar(pos);
    }
    while (pos.col > 0 && isWordChar(charBefore(pos))) {
      pos = prevChar(pos);
    }
    return pos;
  }

  TextRange TextBuffer::wordAt(TextPos pos) const {
    pos = clamp(pos);
    const std::string& line = m_lines[pos.line];
    TextPos from = pos;
    TextPos to = pos;
    while (from.col > 0) {
      const TextPos before = prevChar(from);
      std::size_t at = before.col;
      if (!isWordChar(decodeUtf8(line, at))) {
        break;
      }
      from = before;
    }
    while (to.col < line.size()) {
      std::size_t at = to.col;
      if (!isWordChar(decodeUtf8(line, at))) {
        break;
      }
      to.col = at;
    }
    return {from, to};
  }

  std::optional<TextRange>
  TextBuffer::find(std::string_view needle, TextPos from, bool forward, const SearchOptions& options) const {
    if (needle.empty() || needle.find('\n') != std::string_view::npos) {
      return std::nullopt;
    }
    std::string pattern(needle);
    if (!options.caseSensitive) {
      std::ranges::transform(pattern, pattern.begin(), asciiLower);
    }
    const auto matchesAt = [&](const std::string& line, std::size_t col) {
      if (col + pattern.size() > line.size()) {
        return false;
      }
      for (std::size_t i = 0; i < pattern.size(); ++i) {
        const char c = options.caseSensitive ? line[col + i] : asciiLower(line[col + i]);
        if (c != pattern[i]) {
          return false;
        }
      }
      if (options.wholeWord) {
        if (col > 0) {
          std::size_t before = col - 1;
          while (before > 0 && isContinuation(static_cast<unsigned char>(line[before]))) {
            --before;
          }
          if (isWordChar(decodeUtf8(line, before))) {
            return false;
          }
        }
        std::size_t after = col + pattern.size();
        if (after < line.size() && isWordChar(decodeUtf8(line, after))) {
          return false;
        }
      }
      return true;
    };
    from = clamp(from);
    const std::size_t count = m_lines.size();
    // Recorre todas las líneas una vez desde `from`, dando la vuelta al final (o al principio).
    for (std::size_t step = 0; step <= count; ++step) {
      const std::size_t lineIndex = forward ? (from.line + step) % count : (from.line + count - step % count) % count;
      const std::string& line = m_lines[lineIndex];
      if (forward) {
        const std::size_t start = step == 0 ? from.col : 0;
        for (std::size_t col = start; col + pattern.size() <= line.size(); ++col) {
          if (matchesAt(line, col)) {
            return TextRange{{lineIndex, col}, {lineIndex, col + pattern.size()}};
          }
        }
      } else {
        const std::size_t limit = step == 0 ? from.col : line.size();
        for (std::size_t col = std::min(limit, line.size()) + 1; col-- > 0;) {
          if (col + pattern.size() <= limit && matchesAt(line, col)) {
            return TextRange{{lineIndex, col}, {lineIndex, col + pattern.size()}};
          }
        }
      }
    }
    return std::nullopt;
  }

  std::vector<TextRange> TextBuffer::findAll(std::string_view needle, const SearchOptions& options) const {
    std::vector<TextRange> out;
    TextPos at{0, 0};
    while (auto match = find(needle, at, true, options)) {
      if (!out.empty() && match->from <= out.back().from) {
        break; // dio la vuelta
      }
      out.push_back(*match);
      at = match->to;
    }
    return out;
  }

  std::size_t
  TextBuffer::replaceAll(std::string_view needle, std::string_view replacement, const SearchOptions& options) {
    const auto matches = findAll(needle, options);
    // De atrás hacia adelante para que las posiciones sigan valiendo; un solo grupo de deshacer.
    for (std::size_t i = matches.size(); i-- > 0;) {
      std::string removed = textIn(matches[i]);
      rawErase(matches[i]);
      (void)rawInsert(matches[i].from, replacement);
      record({.at = matches[i].from, .removed = std::move(removed), .inserted = std::string(replacement)}, true);
      m_mergeable = true; // los siguientes reemplazos se suman al mismo paso
    }
    m_mergeable = false;
    return matches.size();
  }

} // namespace okaru::editor
