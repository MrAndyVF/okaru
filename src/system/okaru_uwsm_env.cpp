#include "system/okaru_uwsm_env.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>

namespace okaru::uwsm {

  namespace {

    // Las reescribe cursor-plugin-sync con el tema del plugin de cursor.
    constexpr std::array<std::string_view, 4> kManaged{
        "XCURSOR_THEME", "XCURSOR_SIZE", "HYPRCURSOR_THEME", "HYPRCURSOR_SIZE"
    };

    const std::regex& exportLine() {
      static const std::regex kExport(R"re(^\s*export\s+([A-Za-z_][A-Za-z0-9_]*)=(.*)$)re");
      return kExport;
    }

    std::string unquote(std::string value) {
      while (!value.empty() && (value.back() == ' ' || value.back() == '\r')) {
        value.pop_back();
      }
      if (value.size() >= 2 && (value.front() == '\'' || value.front() == '"') && value.back() == value.front()) {
        return value.substr(1, value.size() - 2);
      }
      return value;
    }

    std::vector<std::string> lines(std::string_view text) {
      std::vector<std::string> out;
      std::istringstream in{std::string(text)};
      std::string line;
      while (std::getline(in, line)) {
        out.push_back(line);
      }
      return out;
    }

    std::string join(const std::vector<std::string>& ls) {
      std::string out;
      for (const auto& l : ls) {
        out += l + "\n";
      }
      return out;
    }

  } // namespace

  std::vector<EnvVar> parseEnv(std::string_view text) {
    std::vector<EnvVar> out;
    std::vector<std::string> comments;
    for (const auto& line : lines(text)) {
      const auto first = line.find_first_not_of(" \t");
      if (first != std::string::npos && line[first] == '#') {
        std::string c = line.substr(first + 1);
        if (c.starts_with(' ')) {
          c.erase(0, 1);
        }
        comments.push_back(std::move(c));
        continue;
      }
      std::smatch m;
      if (std::regex_match(line, m, exportLine())) {
        std::string comment;
        for (const auto& c : comments) {
          comment += (comment.empty() ? "" : " ") + c;
        }
        const std::string key = m[1].str();
        out.push_back({
            .key = key,
            .value = unquote(m[2].str()),
            .comment = std::move(comment),
            .managed = std::ranges::contains(kManaged, std::string_view(key)),
        });
        comments.clear(); // un comentario describe solo a la primera variable de su grupo
      } else if (first == std::string::npos) {
        comments.clear();
      }
    }
    return out;
  }

  bool isValidKey(std::string_view key) {
    return !key.empty()
        && (std::isalpha(static_cast<unsigned char>(key.front())) != 0 || key.front() == '_')
        && std::ranges::all_of(key, [](unsigned char c) { return std::isalnum(c) != 0 || c == '_'; });
  }

  std::string quote(std::string_view value) {
    const bool plain = !value.empty() && std::ranges::all_of(value, [](unsigned char c) {
      return std::isalnum(c) != 0
          || std::string_view("_-./:,=+@%").find(static_cast<char>(c)) != std::string_view::npos;
    });
    if (plain) {
      return std::string(value);
    }
    std::string out = "'";
    for (const char c : value) {
      out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    }
    return out + "'";
  }

  std::string setVar(std::string_view text, std::string_view key, std::string_view value) {
    auto ls = lines(text);
    const std::string line = "export " + std::string(key) + "=" + quote(value);
    for (auto& l : ls) {
      std::smatch m;
      if (std::regex_match(l, m, exportLine()) && m[1].str() == key) {
        l = line;
        return join(ls);
      }
    }
    ls.push_back(line);
    return join(ls);
  }

  std::string removeVar(std::string_view text, std::string_view key) {
    auto ls = lines(text);
    std::erase_if(ls, [&](const std::string& l) {
      std::smatch m;
      return std::regex_match(l, m, exportLine()) && m[1].str() == key;
    });
    return join(ls);
  }

  std::filesystem::path envFile() {
    const char* config = std::getenv("XDG_CONFIG_HOME");
    if (config != nullptr && config[0] == '/') {
      return std::filesystem::path(config) / "uwsm" / "env";
    }
    const char* home = std::getenv("HOME");
    return std::filesystem::path(home != nullptr ? home : "/") / ".config" / "uwsm" / "env";
  }

  std::string readEnvText() {
    std::ifstream file(envFile(), std::ios::binary);
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
  }

  std::optional<std::string> writeEnvText(const std::string& text) {
    const auto path = envFile();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const auto tmp = path.string() + ".okaru-tmp";
    {
      std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
      if (!file || !(file << text)) {
        return "no se pudo escribir " + tmp;
      }
    }
    std::filesystem::rename(tmp, path, ec);
    return ec ? std::optional(ec.message()) : std::nullopt;
  }

} // namespace okaru::uwsm
