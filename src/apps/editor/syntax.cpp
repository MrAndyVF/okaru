#include "apps/editor/syntax.h"

#include <algorithm>
#include <array>
#include <string>

namespace okaru::editor {

  namespace {

    bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r'; }
    bool isDigit(char c) { return c >= '0' && c <= '9'; }
    bool isIdentStart(char c) {
      return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || static_cast<unsigned char>(c) >= 0x80;
    }
    bool isIdent(char c) { return isIdentStart(c) || isDigit(c); }

    // Recorre una línea y junta los tramos coloreados.
    struct Lexer {
      std::string_view line;
      std::vector<Span>& out;
      std::size_t pos = 0;

      [[nodiscard]] bool done() const { return pos >= line.size(); }
      [[nodiscard]] char peek(std::size_t ahead = 0) const {
        return pos + ahead < line.size() ? line[pos + ahead] : '\0';
      }
      [[nodiscard]] bool at(std::string_view text) const { return line.substr(pos).starts_with(text); }
      void add(std::size_t start, std::size_t end, Token token) {
        if (end > start && token != Token::Plain) {
          // Tramos contiguos del mismo tipo se juntan: menos trabajo al dibujar.
          if (!out.empty() && out.back().token == token && out.back().start + out.back().length == start) {
            out.back().length += static_cast<std::uint32_t>(end - start);
          } else {
            out.push_back({static_cast<std::uint32_t>(start), static_cast<std::uint32_t>(end - start), token});
          }
        }
      }
      void emit(std::size_t length, Token token) {
        add(pos, std::min(pos + length, line.size()), token);
        pos = std::min(pos + length, line.size());
      }
      void rest(Token token) { emit(line.size() - pos, token); }
      void skipSpaces() {
        while (!done() && isSpace(peek())) {
          ++pos;
        }
      }
      // Hasta `close` incluido; si no aparece, hasta el final. Devuelve si lo encontró.
      bool until(std::string_view close, Token token) {
        const auto found = line.find(close, pos);
        if (found == std::string_view::npos) {
          rest(token);
          return false;
        }
        emit(found + close.size() - pos, token);
        return true;
      }
      // Cadena con escapes de barra invertida que empieza en `pos` (con la comilla). Devuelve si se cerró.
      bool quoted(Token token) {
        const char quote = peek();
        const std::size_t start = pos++;
        while (!done()) {
          if (peek() == '\\') {
            pos += 2;
            continue;
          }
          if (peek() == quote) {
            ++pos;
            add(start, pos, token);
            return true;
          }
          ++pos;
        }
        pos = line.size();
        add(start, pos, token);
        return false;
      }
      std::size_t identEnd(std::size_t from) const {
        std::size_t end = from;
        while (end < line.size() && isIdent(line[end])) {
          ++end;
        }
        return end;
      }
      std::size_t numberEnd(std::size_t from) const {
        std::size_t end = from;
        while (end < line.size()
               && (isIdent(line[end])
                   || line[end] == '.'
                   || ((line[end] == '+' || line[end] == '-')
                       && end > from
                       && (line[end - 1] == 'e' || line[end - 1] == 'E')))) {
          ++end;
        }
        return end;
      }
      [[nodiscard]] char nextNonSpace(std::size_t from) const {
        while (from < line.size() && isSpace(line[from])) {
          ++from;
        }
        return from < line.size() ? line[from] : '\0';
      }
    };

    // ── HTML y XML ───────────────────────────────────────────────────────────────────────────────────
    enum MarkupState : LineState { MarkupText, MarkupComment, MarkupTag, MarkupCdata, MarkupTagDq, MarkupTagSq };

    LineState markup(Lexer& lx, LineState state) {
      while (!lx.done()) {
        switch (state) {
        case MarkupComment:
          state = lx.until("-->", Token::Comment) ? MarkupText : MarkupComment;
          continue;
        case MarkupCdata:
          state = lx.until("]]>", Token::String) ? MarkupText : MarkupCdata;
          continue;
        case MarkupTagDq:
          state = lx.until("\"", Token::String) ? MarkupTag : MarkupTagDq;
          continue;
        case MarkupTagSq:
          state = lx.until("'", Token::String) ? MarkupTag : MarkupTagSq;
          continue;
        case MarkupTag: {
          lx.skipSpaces();
          if (lx.done()) {
            break;
          }
          const char c = lx.peek();
          if (lx.at("/>") || lx.at("?>")) {
            lx.emit(2, Token::Punctuation);
            state = MarkupText;
          } else if (c == '>') {
            lx.emit(1, Token::Punctuation);
            state = MarkupText;
          } else if (c == '"' || c == '\'') {
            lx.emit(1, Token::String);
            state = lx.until(c == '"' ? "\"" : "'", Token::String) ? MarkupTag : (c == '"' ? MarkupTagDq : MarkupTagSq);
          } else if (c == '=') {
            lx.emit(1, Token::Punctuation);
          } else {
            std::size_t end = lx.pos;
            while (end < lx.line.size()
                   && !isSpace(lx.line[end])
                   && lx.line[end] != '='
                   && lx.line[end] != '>'
                   && !(lx.line[end] == '/' && end + 1 < lx.line.size() && lx.line[end + 1] == '>')) {
              ++end;
            }
            lx.emit(std::max<std::size_t>(1, end - lx.pos), Token::Attribute);
          }
          continue;
        }
        default:
          break;
        }
        // Texto entre etiquetas.
        if (lx.at("<!--")) {
          state = lx.until("-->", Token::Comment) ? MarkupText : MarkupComment;
        } else if (lx.at("<![CDATA[")) {
          state = lx.until("]]>", Token::String) ? MarkupText : MarkupCdata;
        } else if (
            lx.peek() == '<'
            && (lx.peek(1) == '/' || lx.peek(1) == '!' || lx.peek(1) == '?' || isIdentStart(lx.peek(1)))
        ) {
          const bool special = lx.peek(1) == '!' || lx.peek(1) == '?';
          lx.emit(lx.peek(1) == '/' || special ? 2 : 1, Token::Punctuation);
          std::size_t end = lx.pos;
          while (end < lx.line.size()
                 && (isIdent(lx.line[end]) || lx.line[end] == ':' || lx.line[end] == '-' || lx.line[end] == '.')) {
            ++end;
          }
          lx.emit(end - lx.pos, special ? Token::Keyword : Token::Tag);
          state = MarkupTag;
        } else if (lx.peek() == '&') {
          const auto semicolon = lx.line.find(';', lx.pos);
          lx.emit(
              semicolon != std::string_view::npos && semicolon - lx.pos < 12 ? semicolon - lx.pos + 1 : 1,
              Token::Punctuation
          );
        } else {
          const auto next = lx.line.find_first_of("<&", lx.pos + 1);
          lx.pos = next == std::string_view::npos ? lx.line.size() : next;
        }
      }
      return state;
    }

    // ── JSON y JSONC ─────────────────────────────────────────────────────────────────────────────────
    LineState json(Lexer& lx, LineState state) {
      while (!lx.done()) {
        if (state == 1) {
          state = lx.until("*/", Token::Comment) ? 0 : 1;
          continue;
        }
        const char c = lx.peek();
        if (isSpace(c)) {
          ++lx.pos;
        } else if (lx.at("//")) {
          lx.rest(Token::Comment);
        } else if (lx.at("/*")) {
          state = lx.until("*/", Token::Comment) ? 0 : 1;
        } else if (c == '"') {
          // Una cadena seguida de ':' es una clave.
          const std::size_t start = lx.pos;
          std::vector<Span> scratch;
          Lexer inner{lx.line, scratch, start};
          (void)inner.quoted(Token::String);
          lx.add(start, inner.pos, lx.nextNonSpace(inner.pos) == ':' ? Token::Key : Token::String);
          lx.pos = inner.pos;
        } else if (isDigit(c) || (c == '-' && isDigit(lx.peek(1)))) {
          lx.emit(lx.numberEnd(lx.pos + 1) - lx.pos, Token::Number);
        } else if (isIdentStart(c)) {
          const std::size_t end = lx.identEnd(lx.pos);
          const std::string_view word = lx.line.substr(lx.pos, end - lx.pos);
          lx.emit(end - lx.pos, word == "true" || word == "false" || word == "null" ? Token::Constant : Token::Plain);
        } else {
          lx.emit(1, Token::Punctuation);
        }
      }
      return state;
    }

    // ── TOML ─────────────────────────────────────────────────────────────────────────────────────────
    LineState toml(Lexer& lx, LineState state) {
      if (state == 1) {
        if (!lx.until(R"(""")", Token::String)) {
          return 1;
        }
      } else if (state == 2) {
        if (!lx.until("'''", Token::String)) {
          return 2;
        }
      }
      lx.skipSpaces();
      if (lx.pos == 0 || state == 0) {
        if (lx.peek() == '[') {
          const auto close = lx.line.find(']', lx.pos);
          const std::size_t end = close == std::string_view::npos
              ? lx.line.size()
              : close + (lx.line.substr(close).starts_with("]]") ? 2 : 1);
          lx.emit(end - lx.pos, Token::Section);
        }
      }
      bool expectKey = true;
      int braces = 0;
      while (!lx.done()) {
        const char c = lx.peek();
        if (isSpace(c)) {
          ++lx.pos;
        } else if (c == '#') {
          lx.rest(Token::Comment);
        } else if (lx.at(R"(""")")) {
          lx.emit(3, Token::String);
          if (!lx.until(R"(""")", Token::String)) {
            return 1;
          }
        } else if (lx.at("'''")) {
          lx.emit(3, Token::String);
          if (!lx.until("'''", Token::String)) {
            return 2;
          }
        } else if (c == '"' || c == '\'') {
          const std::size_t start = lx.pos;
          if (c == '\'') {
            ++lx.pos;
            (void)lx.until("'", Token::Plain);
          } else {
            std::vector<Span> scratch;
            Lexer inner{lx.line, scratch, start};
            (void)inner.quoted(Token::String);
            lx.pos = inner.pos;
          }
          lx.add(start, lx.pos, expectKey && lx.nextNonSpace(lx.pos) != ',' ? Token::Key : Token::String);
        } else if (c == '=') {
          lx.emit(1, Token::Punctuation);
          expectKey = false;
        } else if (c == '{') {
          lx.emit(1, Token::Punctuation);
          ++braces;
          expectKey = true;
        } else if (c == ',') {
          lx.emit(1, Token::Punctuation);
          expectKey = braces > 0;
        } else if (expectKey && (isIdent(c) || c == '-' || c == '.')) {
          std::size_t end = lx.pos;
          while (end < lx.line.size() && (isIdent(lx.line[end]) || lx.line[end] == '-' || lx.line[end] == '.')) {
            ++end;
          }
          lx.emit(end - lx.pos, Token::Key);
        } else if (isDigit(c) || ((c == '-' || c == '+') && isDigit(lx.peek(1)))) {
          std::size_t end = lx.pos + 1;
          while (end < lx.line.size()
                 && (isIdent(lx.line[end])
                     || lx.line[end] == '.'
                     || lx.line[end] == ':'
                     || lx.line[end] == '-'
                     || lx.line[end] == '+'
                     || (lx.line[end] == ' '
                         && end + 1 < lx.line.size()
                         && isDigit(lx.line[end + 1])
                         && lx.line[end - 1] != ','))) {
            ++end;
          }
          lx.emit(end - lx.pos, Token::Number);
        } else if (isIdentStart(c)) {
          const std::size_t end = lx.identEnd(lx.pos);
          const std::string_view word = lx.line.substr(lx.pos, end - lx.pos);
          lx.emit(
              end - lx.pos,
              word == "true" || word == "false"    ? Token::Constant
                  : word == "inf" || word == "nan" ? Token::Number
                                                   : Token::Plain
          );
        } else {
          if (c == '}') {
            braces = std::max(0, braces - 1);
          }
          lx.emit(1, Token::Punctuation);
        }
      }
      return 0;
    }

    // ── CSS ──────────────────────────────────────────────────────────────────────────────────────────
    LineState css(Lexer& lx, LineState state) {
      bool comment = (state & 1U) != 0;
      LineState depth = state >> 1U;
      bool value = false; // después de "propiedad:" hasta ';'
      while (!lx.done()) {
        if (comment) {
          comment = !lx.until("*/", Token::Comment);
          continue;
        }
        const char c = lx.peek();
        if (isSpace(c)) {
          ++lx.pos;
        } else if (lx.at("/*")) {
          comment = !lx.until("*/", Token::Comment);
        } else if (c == '"' || c == '\'') {
          (void)lx.quoted(Token::String);
        } else if (c == '{') {
          lx.emit(1, Token::Punctuation);
          ++depth;
          value = false;
        } else if (c == '}') {
          lx.emit(1, Token::Punctuation);
          depth = depth > 0 ? depth - 1 : 0;
          value = false;
        } else if (c == ';') {
          lx.emit(1, Token::Punctuation);
          value = false;
        } else if (c == '@') {
          lx.emit(lx.identEnd(lx.pos + 1) - lx.pos, Token::Keyword);
        } else if (depth == 0) {
          // Selector: hasta '{' o ',' (los pseudo :hover forman parte).
          std::size_t end = lx.pos;
          while (end < lx.line.size()
                 && lx.line[end] != '{'
                 && lx.line[end] != ','
                 && !lx.line.substr(end).starts_with("/*")) {
            ++end;
          }
          while (end > lx.pos && isSpace(lx.line[end - 1])) {
            --end;
          }
          lx.emit(std::max<std::size_t>(1, end - lx.pos), c == ',' ? Token::Punctuation : Token::Tag);
        } else if (!value && (isIdentStart(c) || c == '-')) {
          std::size_t end = lx.pos;
          while (end < lx.line.size() && (isIdent(lx.line[end]) || lx.line[end] == '-')) {
            ++end;
          }
          const char after = lx.nextNonSpace(end);
          // "color:" es propiedad; "a:hover {" anidado es selector.
          const bool property = after == ':' && lx.line.find('{', end) == std::string_view::npos;
          lx.emit(end - lx.pos, property ? Token::Key : Token::Tag);
        } else if (c == ':') {
          lx.emit(1, Token::Punctuation);
          value = true;
        } else if (c == '#' && value) {
          lx.emit(lx.identEnd(lx.pos + 1) - lx.pos, Token::Number);
        } else if (isDigit(c) || ((c == '-' || c == '.') && isDigit(lx.peek(1)))) {
          std::size_t end = lx.pos + 1;
          while (end < lx.line.size() && (isIdent(lx.line[end]) || lx.line[end] == '.' || lx.line[end] == '%')) {
            ++end;
          }
          lx.emit(end - lx.pos, Token::Number);
        } else if (c == '!') {
          lx.emit(lx.identEnd(lx.pos + 1) - lx.pos, Token::Keyword);
        } else if (isIdentStart(c) || c == '-') {
          std::size_t end = lx.pos;
          while (end < lx.line.size() && (isIdent(lx.line[end]) || lx.line[end] == '-')) {
            ++end;
          }
          lx.emit(end - lx.pos, end < lx.line.size() && lx.line[end] == '(' ? Token::Function : Token::Plain);
        } else {
          lx.emit(1, Token::Punctuation);
        }
      }
      return (std::min<LineState>(depth, 0x7FFF) << 1U) | (comment ? 1U : 0U);
    }

    // ── conf / ini / env ─────────────────────────────────────────────────────────────────────────────
    LineState conf(Lexer& lx) {
      lx.skipSpaces();
      const char first = lx.peek();
      if (first == '#' || first == ';') {
        lx.rest(Token::Comment);
        return 0;
      }
      if (first == '[') {
        const auto close = lx.line.find(']', lx.pos);
        lx.emit((close == std::string_view::npos ? lx.line.size() : close + 1) - lx.pos, Token::Section);
        lx.skipSpaces();
        if (lx.peek() == '#' || lx.peek() == ';') {
          lx.rest(Token::Comment);
        }
        return 0;
      }
      const auto separator = lx.line.find_first_of("=:", lx.pos);
      if (separator == std::string_view::npos) {
        return 0;
      }
      std::size_t keyEnd = separator;
      while (keyEnd > lx.pos && isSpace(lx.line[keyEnd - 1])) {
        --keyEnd;
      }
      // "export CLAVE=valor" de los archivos de shell.
      if (lx.line.substr(lx.pos).starts_with("export ")) {
        lx.emit(6, Token::Keyword);
        lx.skipSpaces();
      }
      lx.emit(keyEnd - lx.pos, Token::Key);
      lx.pos = separator;
      lx.emit(1, Token::Punctuation);
      while (!lx.done()) {
        const char c = lx.peek();
        if (isSpace(c)) {
          ++lx.pos;
          if (lx.peek() == '#' || lx.peek() == ';') {
            lx.rest(Token::Comment);
          }
        } else if (c == '"' || c == '\'') {
          (void)lx.quoted(Token::String);
        } else if (isDigit(c) || (c == '-' && isDigit(lx.peek(1)))) {
          lx.emit(lx.numberEnd(lx.pos + 1) - lx.pos, Token::Number);
        } else if (isIdentStart(c)) {
          const std::size_t end = lx.identEnd(lx.pos);
          std::string word(lx.line.substr(lx.pos, end - lx.pos));
          std::ranges::transform(word, word.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
          });
          constexpr std::array<std::string_view, 8> kConstants{"true", "false", "yes",  "no",
                                                               "on",   "off",   "none", "null"};
          lx.emit(end - lx.pos, std::ranges::contains(kConstants, word) ? Token::Constant : Token::Plain);
        } else {
          ++lx.pos;
        }
      }
      return 0;
    }

    // ── Lua ──────────────────────────────────────────────────────────────────────────────────────────
    constexpr LineState kLuaString = 0x100;
    constexpr LineState kLuaComment = 0x200;

    // Nivel de "[==[" en `pos`, o -1 si no es un corchete largo.
    int longBracket(std::string_view line, std::size_t pos) {
      if (pos >= line.size() || line[pos] != '[') {
        return -1;
      }
      std::size_t end = pos + 1;
      while (end < line.size() && line[end] == '=') {
        ++end;
      }
      return end < line.size() && line[end] == '[' ? static_cast<int>(end - pos - 1) : -1;
    }

    LineState lua(Lexer& lx, LineState state) {
      constexpr std::array<std::string_view, 19> kKeywords{
          "and", "break", "do",  "else", "elseif", "end",    "for",  "function", "goto",  "if",
          "in",  "local", "not", "or",   "repeat", "return", "then", "until",    "while",
      };
      const auto closeLong = [&](LineState level, Token token) {
        const std::string close = "]" + std::string(level, '=') + "]";
        return lx.until(close, token);
      };
      if ((state & kLuaString) != 0 && !closeLong(state & 0xFFU, Token::String)) {
        return state;
      }
      if ((state & kLuaComment) != 0 && !closeLong(state & 0xFFU, Token::Comment)) {
        return state;
      }
      bool afterFunction = false;
      while (!lx.done()) {
        const char c = lx.peek();
        if (isSpace(c)) {
          ++lx.pos;
        } else if (lx.at("--")) {
          const int level = longBracket(lx.line, lx.pos + 2);
          if (level >= 0) {
            lx.emit(static_cast<std::size_t>(level) + 4, Token::Comment);
            if (!closeLong(static_cast<LineState>(level), Token::Comment)) {
              return kLuaComment | static_cast<LineState>(level);
            }
          } else {
            lx.rest(Token::Comment);
          }
        } else if (const int level = longBracket(lx.line, lx.pos); level >= 0) {
          lx.emit(static_cast<std::size_t>(level) + 2, Token::String);
          if (!closeLong(static_cast<LineState>(level), Token::String)) {
            return kLuaString | static_cast<LineState>(level);
          }
        } else if (c == '"' || c == '\'') {
          (void)lx.quoted(Token::String);
        } else if (isDigit(c) || (c == '.' && isDigit(lx.peek(1)))) {
          lx.emit(lx.numberEnd(lx.pos + 1) - lx.pos, Token::Number);
        } else if (isIdentStart(c)) {
          const std::size_t end = lx.identEnd(lx.pos);
          const std::string_view word = lx.line.substr(lx.pos, end - lx.pos);
          Token token = Token::Plain;
          if (word == "true" || word == "false" || word == "nil") {
            token = Token::Constant;
          } else if (std::ranges::contains(kKeywords, word)) {
            token = Token::Keyword;
          } else if (afterFunction || lx.nextNonSpace(end) == '(') {
            token = Token::Function;
          }
          afterFunction = word == "function";
          lx.emit(end - lx.pos, token);
        } else {
          lx.emit(1, Token::Punctuation);
        }
      }
      return 0;
    }

    // ── KDL ──────────────────────────────────────────────────────────────────────────────────────────
    LineState kdl(Lexer& lx, LineState depth) {
      bool nodeStart = depth == 0;
      while (!lx.done()) {
        if (depth > 0) {
          // Comentarios /* */ anidados.
          const auto open = lx.line.find("/*", lx.pos);
          const auto close = lx.line.find("*/", lx.pos);
          if (close == std::string_view::npos && open == std::string_view::npos) {
            lx.rest(Token::Comment);
            return depth;
          }
          if (open != std::string_view::npos && (close == std::string_view::npos || open < close)) {
            lx.emit(open + 2 - lx.pos, Token::Comment);
            ++depth;
          } else {
            lx.emit(close + 2 - lx.pos, Token::Comment);
            --depth;
            nodeStart = false;
          }
          continue;
        }
        const char c = lx.peek();
        if (isSpace(c)) {
          ++lx.pos;
        } else if (lx.at("//")) {
          lx.rest(Token::Comment);
        } else if (lx.at("/*")) {
          lx.emit(2, Token::Comment);
          depth = 1;
        } else if (lx.at("/-")) {
          lx.emit(2, Token::Comment);
        } else if (c == '{' || c == ';') {
          lx.emit(1, Token::Punctuation);
          nodeStart = true;
        } else if (c == '}' || c == '=') {
          lx.emit(1, Token::Punctuation);
        } else if (c == '(') {
          const auto close = lx.line.find(')', lx.pos);
          lx.emit((close == std::string_view::npos ? lx.line.size() : close + 1) - lx.pos, Token::Keyword);
        } else if (c == '#' && (lx.peek(1) == '"' || lx.peek(1) == '#')) {
          // Cadena cruda #"…"#.
          std::size_t hashes = 0;
          while (lx.peek(hashes) == '#') {
            ++hashes;
          }
          const std::string close = "\"" + std::string(hashes, '#');
          lx.emit(hashes + 1, Token::String);
          (void)lx.until(close, Token::String);
          nodeStart = false;
        } else if (c == '#') {
          lx.emit(lx.identEnd(lx.pos + 1) - lx.pos, Token::Constant);
        } else if (c == '"') {
          const std::size_t start = lx.pos;
          std::vector<Span> scratch;
          Lexer inner{lx.line, scratch, start};
          (void)inner.quoted(Token::String);
          lx.add(
              start, inner.pos,
              nodeStart ? Token::Tag
                        : (inner.pos < lx.line.size() && lx.line[inner.pos] == '=' ? Token::Attribute : Token::String)
          );
          lx.pos = inner.pos;
          nodeStart = false;
        } else if (isDigit(c) || ((c == '-' || c == '+') && isDigit(lx.peek(1)))) {
          lx.emit(lx.numberEnd(lx.pos + 1) - lx.pos, Token::Number);
          nodeStart = false;
        } else {
          std::size_t end = lx.pos;
          while (end < lx.line.size()
                 && !isSpace(lx.line[end])
                 && std::string_view("{}();=\"/\\").find(lx.line[end]) == std::string_view::npos) {
            ++end;
          }
          end = std::max(end, lx.pos + 1);
          const std::string_view word = lx.line.substr(lx.pos, end - lx.pos);
          Token token = Token::Plain;
          if (nodeStart) {
            token = Token::Tag;
          } else if (end < lx.line.size() && lx.line[end] == '=') {
            token = Token::Attribute;
          } else if (word == "true" || word == "false" || word == "null") {
            token = Token::Constant; // KDL v1
          }
          lx.emit(end - lx.pos, token);
          nodeStart = false;
        }
      }
      return depth;
    }

    // ── CSV ──────────────────────────────────────────────────────────────────────────────────────────
    constexpr LineState kCsvQuoted = 1U << 16U;

    LineState csv(Lexer& lx, LineState state) {
      // Separador: el primero que aparezca de , ; tab (CSV de hojas de cálculo en español usan ;).
      char delimiter = ',';
      for (const char candidate : {',', ';', '\t'}) {
        if (lx.line.find(candidate) != std::string_view::npos) {
          delimiter = candidate;
          break;
        }
      }
      LineState column = state & 0xFFFFU;
      bool quoted = (state & kCsvQuoted) != 0;
      const auto token = [&] { return static_cast<Token>(static_cast<int>(Token::Column0) + column % 6); };
      std::size_t fieldStart = 0;
      while (lx.pos < lx.line.size()) {
        const char c = lx.line[lx.pos];
        if (quoted) {
          if (c == '"' && lx.peek(1) == '"') {
            lx.pos += 2;
          } else {
            quoted = c != '"';
            ++lx.pos;
          }
        } else if (c == '"') {
          quoted = true;
          ++lx.pos;
        } else if (c == delimiter) {
          lx.add(fieldStart, lx.pos, token());
          lx.add(lx.pos, lx.pos + 1, Token::Punctuation);
          ++lx.pos;
          fieldStart = lx.pos;
          ++column;
        } else {
          ++lx.pos;
        }
      }
      lx.add(fieldStart, lx.pos, token());
      return quoted ? (kCsvQuoted | (column & 0xFFFFU)) : 0;
    }

    struct LanguageInfo {
      Language language;
      std::string_view id;
      std::string_view lineComment;
    };

    constexpr std::array<LanguageInfo, kLanguageCount> kLanguages{{
        {Language::Text, "text", ""},
        {Language::Html, "html", ""},
        {Language::Xml, "xml", ""},
        {Language::Json, "json", ""},
        {Language::Jsonc, "jsonc", "//"},
        {Language::Toml, "toml", "#"},
        {Language::Css, "css", ""},
        {Language::Conf, "conf", "#"},
        {Language::Lua, "lua", "--"},
        {Language::Kdl, "kdl", "//"},
        {Language::Csv, "csv", ""},
    }};

  } // namespace

  LineState highlightLine(Language language, std::string_view line, LineState in, std::vector<Span>& out) {
    out.clear();
    Lexer lx{line, out};
    switch (language) {
    case Language::Html:
    case Language::Xml:
      return markup(lx, in);
    case Language::Json:
    case Language::Jsonc:
      return json(lx, in);
    case Language::Toml:
      return toml(lx, in);
    case Language::Css:
      return css(lx, in);
    case Language::Conf:
      return conf(lx);
    case Language::Lua:
      return lua(lx, in);
    case Language::Kdl:
      return kdl(lx, in);
    case Language::Csv:
      return csv(lx, in);
    case Language::Text:
      break;
    }
    return 0;
  }

  Language detectLanguage(const std::filesystem::path& path, std::string_view firstLine) {
    std::string ext = path.extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const std::string name = path.filename().string();
    struct ByExtension {
      std::string_view ext;
      Language language;
    };
    constexpr std::array<ByExtension, 32> kExtensions{{
        {".html", Language::Html},       {".htm", Language::Html},     {".xhtml", Language::Html},
        {".xml", Language::Xml},         {".svg", Language::Xml},      {".xsd", Language::Xml},
        {".xsl", Language::Xml},         {".plist", Language::Xml},    {".ui", Language::Xml},
        {".json", Language::Json},       {".geojson", Language::Json}, {".webmanifest", Language::Json},
        {".jsonc", Language::Jsonc},     {".json5", Language::Jsonc},  {".toml", Language::Toml},
        {".css", Language::Css},         {".scss", Language::Css},     {".conf", Language::Conf},
        {".cfg", Language::Conf},        {".ini", Language::Conf},     {".env", Language::Conf},
        {".properties", Language::Conf}, {".desktop", Language::Conf}, {".service", Language::Conf},
        {".rc", Language::Conf},         {".lua", Language::Lua},      {".kdl", Language::Kdl},
        {".csv", Language::Csv},         {".tsv", Language::Csv},      {".txt", Language::Text},
        {".log", Language::Text},        {".md", Language::Text},
    }};
    // JSON con comentarios aunque la extensión diga .json (VS Code, TypeScript).
    if (name == "settings.json"
        || name == "keybindings.json"
        || name == "tsconfig.json"
        || name.ends_with(".code-workspace")) {
      return Language::Jsonc;
    }
    for (const auto& entry : kExtensions) {
      if (ext == entry.ext) {
        return entry.language;
      }
    }
    // Archivos de configuración sin extensión: .bashrc, .gitconfig, hyprland.conf ya entra arriba…
    if (name.starts_with('.') && ext.empty()) {
      return Language::Conf;
    }
    if (firstLine.starts_with("<?xml")) {
      return Language::Xml;
    }
    if (firstLine.starts_with("<!DOCTYPE html")
        || firstLine.starts_with("<!doctype html")
        || firstLine.starts_with("<html")) {
      return Language::Html;
    }
    if (firstLine.starts_with("#!")) {
      return Language::Conf;
    }
    return Language::Text;
  }

  std::string_view languageId(Language language) { return kLanguages[static_cast<std::size_t>(language)].id; }

  std::string_view lineComment(Language language) { return kLanguages[static_cast<std::size_t>(language)].lineComment; }

} // namespace okaru::editor
