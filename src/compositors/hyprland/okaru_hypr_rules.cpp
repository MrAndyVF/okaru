#include "compositors/hyprland/okaru_hypr_rules.h"

#include "compositors/hyprland/hyprland_runtime.h"
#include "compositors/hyprland/okaru_hypr_options.h"

#include <cctype>
#include <format>
#include <fstream>
#include <sstream>
#include <system_error>

namespace okaru::hypr {
  namespace {

    // Lector recursivo de tablas Lua literales.
    struct Reader {
      std::string_view s;
      std::size_t i = 0;

      void skip() {
        while (i < s.size()) {
          if (std::isspace(static_cast<unsigned char>(s[i])) != 0) {
            ++i;
          } else if (s.substr(i, 2) == "--") { // comentario hasta fin de línea
            while (i < s.size() && s[i] != '\n') {
              ++i;
            }
          } else {
            break;
          }
        }
      }

      std::optional<std::string> string() {
        const char quote = s[i];
        std::string out;
        for (++i; i < s.size(); ++i) {
          const char c = s[i];
          if (c == '\\' && i + 1 < s.size()) {
            const char n = s[++i];
            out += n == 'n' ? '\n' : (n == 't' ? '\t' : n);
          } else if (c == quote) {
            ++i;
            return out;
          } else {
            out += c;
          }
        }
        return std::nullopt;
      }

      std::optional<nlohmann::ordered_json> value() {
        skip();
        if (i >= s.size()) {
          return std::nullopt;
        }
        const char c = s[i];
        if (c == '{') {
          return table();
        }
        if (c == '"' || c == '\'') {
          auto str = string();
          return str ? std::optional<nlohmann::ordered_json>(*str) : std::nullopt;
        }
        if (s.substr(i, 4) == "true") {
          i += 4;
          return nlohmann::ordered_json(true);
        }
        if (s.substr(i, 5) == "false") {
          i += 5;
          return nlohmann::ordered_json(false);
        }
        std::size_t j = i;
        while (j < s.size()
               && (std::isdigit(static_cast<unsigned char>(s[j])) != 0
                   || s[j] == '-'
                   || s[j] == '.'
                   || s[j] == 'e'
                   || s[j] == 'E'
                   || s[j] == '+')) {
          ++j;
        }
        if (j == i) {
          return std::nullopt;
        }
        const std::string number(s.substr(i, j - i));
        i = j;
        if (number.find_first_of(".eE") != std::string::npos) {
          return nlohmann::ordered_json(std::strtod(number.c_str(), nullptr));
        }
        return nlohmann::ordered_json(std::strtoll(number.c_str(), nullptr, 10));
      }

      std::optional<nlohmann::ordered_json> table() {
        ++i; // {
        nlohmann::ordered_json object = nlohmann::ordered_json::object();
        nlohmann::ordered_json array = nlohmann::ordered_json::array();
        while (true) {
          skip();
          if (i >= s.size()) {
            return std::nullopt;
          }
          if (s[i] == '}') {
            ++i;
            break;
          }
          // clave = valor | ["clave"] = valor | valor
          std::size_t save = i;
          std::optional<std::string> key;
          if (s[i] == '[') {
            ++i;
            skip();
            if (i < s.size() && (s[i] == '"' || s[i] == '\'')) {
              key = string();
            }
            skip();
            if (!key || i >= s.size() || s[i] != ']') {
              return std::nullopt;
            }
            ++i;
          } else if (std::isalpha(static_cast<unsigned char>(s[i])) != 0 || s[i] == '_') {
            std::size_t j = i;
            while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) != 0 || s[j] == '_')) {
              ++j;
            }
            key = std::string(s.substr(i, j - i));
            i = j;
          }
          if (key) {
            skip();
            if (i < s.size() && s[i] == '=') {
              ++i;
            } else {
              key.reset(); // era un valor (true/false) en una lista
              i = save;
            }
          }
          auto v = value();
          if (!v) {
            return std::nullopt;
          }
          if (key) {
            object[*key] = std::move(*v);
          } else {
            array.push_back(std::move(*v));
          }
          skip();
          if (i < s.size() && (s[i] == ',' || s[i] == ';')) {
            ++i;
          }
        }
        if (!array.empty() && object.empty()) {
          return array;
        }
        return object;
      }
    };

    std::string luaString(std::string_view text) {
      std::string out = "\"";
      for (const char c : text) {
        if (c == '"' || c == '\\') {
          out += '\\';
        }
        out += c;
      }
      return out + "\"";
    }

    bool isIdentifier(std::string_view key) {
      return !key.empty()
          && (std::isalpha(static_cast<unsigned char>(key[0])) != 0 || key[0] == '_')
          && std::ranges::all_of(key, [](char c) {
               return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
             });
    }

    // Fin de la llamada que empieza en `open` (el paréntesis), respetando strings y llaves.
    std::optional<std::size_t> callEnd(std::string_view s, std::size_t open) {
      int depth = 0;
      for (std::size_t i = open; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '"' || c == '\'') {
          for (++i; i < s.size() && s[i] != c; ++i) {
            if (s[i] == '\\') {
              ++i;
            }
          }
        } else if (c == '-' && s.substr(i, 2) == "--") {
          while (i < s.size() && s[i] != '\n') {
            ++i;
          }
        } else if (c == '(' || c == '{') {
          ++depth;
        } else if (c == ')' || c == '}') {
          if (--depth == 0) {
            return i + 1;
          }
        }
      }
      return std::nullopt;
    }

    std::string readFile(const std::filesystem::path& path) {
      std::ifstream in(path, std::ios::binary);
      if (!in) {
        return {};
      }
      std::stringstream buffer;
      buffer << in.rdbuf();
      return buffer.str();
    }

    std::optional<std::string> writeAtomic(const std::filesystem::path& path, const std::string& text) {
      const auto tmp = path.string() + ".okaru-tmp";
      {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out || !(out << text) || !out.flush()) {
          return "no se pudo escribir " + tmp;
        }
      }
      std::error_code ec;
      std::filesystem::rename(tmp, path, ec);
      if (ec) {
        std::filesystem::remove(tmp, ec);
        return "no se pudo reemplazar " + path.string();
      }
      return std::nullopt;
    }

    // Inicio de la línea que contiene `pos`.
    std::size_t lineStart(std::string_view s, std::size_t pos) {
      const auto nl = s.rfind('\n', pos == 0 ? 0 : pos - 1);
      return pos == 0 || nl == std::string_view::npos ? 0 : nl + 1;
    }

  } // namespace

  std::optional<nlohmann::ordered_json> parseLuaTable(std::string_view text) {
    Reader reader{.s = text};
    reader.skip();
    if (reader.i >= text.size() || text[reader.i] != '{') {
      return std::nullopt;
    }
    auto table = reader.table();
    reader.skip();
    if (!table || reader.i != text.size()) {
      return std::nullopt;
    }
    return table;
  }

  std::string toLuaTable(const nlohmann::ordered_json& value) {
    if (value.is_string()) {
      return luaString(value.get<std::string>());
    }
    if (value.is_boolean()) {
      return value.get<bool>() ? "true" : "false";
    }
    if (value.is_number_integer()) {
      return std::to_string(value.get<long long>());
    }
    if (value.is_number()) {
      std::string text = std::format("{:.6f}", value.get<double>());
      while (text.ends_with('0')) {
        text.pop_back();
      }
      if (text.ends_with('.')) {
        text.pop_back();
      }
      return text;
    }
    std::string out = "{ ";
    bool first = true;
    if (value.is_array()) {
      for (const auto& item : value) {
        out += (first ? "" : ", ") + toLuaTable(item);
        first = false;
      }
    } else if (value.is_object()) {
      // En el orden original (ordered_json): una regla editada se parece a como estaba escrita.
      for (const auto& [key, item] : value.items()) {
        out += (first ? "" : ", ") + (isIdentifier(key) ? key : "[" + luaString(key) + "]") + " = " + toLuaTable(item);
        first = false;
      }
    }
    return first ? "{}" : out + " }";
  }

  std::vector<Rule> parseRulesFile(std::string_view text) {
    std::vector<Rule> rules;
    std::size_t pos = 0;
    while (true) {
      const auto window = text.find("hl.window_rule(", pos);
      const auto layer = text.find("hl.layer_rule(", pos);
      const auto at = std::min(window, layer);
      if (at == std::string_view::npos) {
        break;
      }
      const bool isWindow = at == window;
      const auto open = text.find('(', at);
      const auto end = callEnd(text, open);
      if (!end) {
        break;
      }
      pos = *end;
      // Solo llamadas al inicio de una línea (no dentro de comentarios ni de otro código).
      const auto start = lineStart(text, at);
      if (text.substr(start, at - start).find_first_not_of(" \t") != std::string_view::npos) {
        continue;
      }
      auto table = parseLuaTable(text.substr(open + 1, *end - open - 2));
      if (!table || !table->is_object()) {
        continue;
      }
      Rule rule{
          .kind = isWindow ? RuleKind::Window : RuleKind::Layer, .table = std::move(*table), .start = at, .end = *end
      };
      // Comentario: las líneas "-- ..." justo encima.
      std::vector<std::string> lines;
      std::size_t cursor = start;
      while (cursor > 0) {
        const auto prevStart = lineStart(text, cursor - 1);
        std::string_view line = text.substr(prevStart, cursor - 1 - prevStart);
        const auto first = line.find_first_not_of(" \t");
        if (first == std::string_view::npos || !line.substr(first).starts_with("--")) {
          break;
        }
        std::string_view body = line.substr(first + 2);
        while (!body.empty() && body.front() == ' ') {
          body.remove_prefix(1);
        }
        lines.insert(lines.begin(), std::string(body));
        cursor = prevStart;
      }
      rule.commentStart = lines.empty() ? start : cursor;
      for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i == 0) {
          rule.title = lines[i];
        } else {
          rule.description += (rule.description.empty() ? "" : " ") + lines[i];
        }
      }
      rules.push_back(std::move(rule));
    }
    return rules;
  }

  std::string serializeRuleCall(RuleKind kind, const nlohmann::ordered_json& table) {
    return std::string(kind == RuleKind::Window ? "hl.window_rule(" : "hl.layer_rule(") + toLuaTable(table) + ")";
  }

  std::string replaceRule(std::string_view text, const Rule& rule, std::string_view call) {
    std::string out(text);
    return out.replace(rule.start, rule.end - rule.start, call);
  }

  std::string removeRule(std::string_view text, const Rule& rule) {
    std::string out(text);
    std::size_t start = lineStart(text, rule.start);
    std::size_t end = rule.end;
    if (end < out.size() && out[end] == '\n') {
      ++end;
    }
    // El comentario de arriba se va con la regla.
    while (start > 0) {
      const auto prev = lineStart(text, start - 1);
      std::string_view line = text.substr(prev, start - 1 - prev);
      const auto first = line.find_first_not_of(" \t");
      if (first == std::string_view::npos || !line.substr(first).starts_with("--")) {
        break;
      }
      start = prev;
    }
    // Y una línea en blanco que separaba si queda doble.
    if (start >= 2 && out[start - 1] == '\n' && out[start - 2] == '\n' && (end >= out.size() || out[end] == '\n')) {
      --start;
    }
    return out.erase(start, end - start);
  }

  namespace {
    // "-- título\n-- descripción\n": una línea de comentario por línea de texto; las vacías se omiten.
    std::string commentBlock(std::string_view title, std::string_view description) {
      std::string out;
      for (const auto part : {title, description}) {
        std::size_t pos = 0;
        while (pos <= part.size()) {
          const auto nl = part.find('\n', pos);
          std::string_view line = part.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
          while (!line.empty() && (line.back() == ' ' || line.back() == '\r')) {
            line.remove_suffix(1);
          }
          if (!line.empty()) {
            out += "-- " + std::string(line) + "\n";
          }
          if (nl == std::string_view::npos) {
            break;
          }
          pos = nl + 1;
        }
      }
      return out;
    }
  } // namespace

  std::string replaceRuleWithComment(
      std::string_view text, const Rule& rule, std::string_view title, std::string_view description,
      std::string_view call
  ) {
    std::string out(text);
    const std::size_t ruleLine = lineStart(text, rule.start);
    const std::size_t from = std::min(rule.commentStart, ruleLine);
    const std::string indent(text.substr(ruleLine, rule.start - ruleLine)); // conserva la sangría
    return out.replace(from, rule.end - from, commentBlock(title, description) + indent + std::string(call));
  }

  std::string
  appendRule(std::string_view text, std::string_view title, std::string_view description, std::string_view call) {
    std::string out(text);
    if (!out.empty() && !out.ends_with('\n')) {
      out += '\n';
    }
    out += '\n';
    return out + commentBlock(title, description) + std::string(call) + "\n";
  }

  std::optional<std::size_t> luaCallEnd(std::string_view text, std::size_t openParen) {
    return callEnd(text, openParen);
  }
  std::size_t luaLineStart(std::string_view text, std::size_t pos) { return lineStart(text, pos); }
  std::optional<std::string> writeFileAtomic(const std::filesystem::path& path, const std::string& text) {
    return writeAtomic(path, text);
  }

  std::filesystem::path rulesFilePath() { return okaFilePath("windowrules"); }

  std::string readRulesText() { return readFile(rulesFilePath()); }

  std::optional<std::string> writeRulesText(
      compositors::hyprland::HyprlandRuntime& runtime, const std::string& previous, const std::string& next
  ) {
    const auto path = rulesFilePath();
    if (auto err = writeAtomic(path, next)) {
      return err;
    }
    (void)runtime.request("reload");
    const auto errors = runtime.request("configerrors");
    if (errors.has_value() && errors->find("windowrules.lua") != std::string::npos) {
      (void)writeAtomic(path, previous);
      (void)runtime.request("reload");
      std::string message = *errors;
      while (!message.empty() && (message.back() == '\n' || message.back() == ' ')) {
        message.pop_back();
      }
      return message;
    }
    return std::nullopt;
  }

  std::vector<OpenWindow> queryOpenWindows(compositors::hyprland::HyprlandRuntime& runtime) {
    std::vector<OpenWindow> windows;
    const auto json = runtime.requestJson("j/clients");
    if (!json || !json->is_array()) {
      return windows;
    }
    for (const auto& c : *json) {
      if (!c.is_object()) {
        continue;
      }
      OpenWindow w{.cls = c.value("class", ""), .title = c.value("title", "")};
      if (!w.cls.empty() && !std::ranges::contains(windows, w.cls, &OpenWindow::cls)) {
        windows.push_back(std::move(w));
      }
    }
    return windows;
  }

  std::string exactRegex(std::string_view text) {
    std::string out = "^(";
    for (const char c : text) {
      if (std::string_view(".^$|()[]{}*+?\\").find(c) != std::string_view::npos) {
        out += '\\';
      }
      out += c;
    }
    return out + ")$";
  }

} // namespace okaru::hypr
