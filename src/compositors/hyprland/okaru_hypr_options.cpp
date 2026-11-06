#include "compositors/hyprland/okaru_hypr_options.h"

#include "compositors/hyprland/hyprland_runtime.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <format>
#include <fstream>
#include <regex>
#include <sstream>
#include <system_error>

namespace okaru::hypr {
  namespace {

    constexpr std::string_view kBegin = "-- okaru:options:begin";
    constexpr std::string_view kEnd = "-- okaru:options:end";

    bool isIdentifier(std::string_view key) {
      return !key.empty()
          && (std::isalpha(static_cast<unsigned char>(key[0])) != 0 || key[0] == '_')
          && std::ranges::all_of(key, [](char c) {
               return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
             });
    }

    std::string luaKey(std::string_view key) {
      return isIdentifier(key) ? std::string(key) : std::format("[\"{}\"]", key);
    }

    std::string luaValue(const nlohmann::json& value) {
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
      std::string out = "\"";
      for (const char c : value.is_string() ? value.get<std::string>() : value.dump()) {
        if (c == '"' || c == '\\') {
          out += '\\';
        }
        out += c;
      }
      return out + "\"";
    }

    std::vector<std::string> splitName(std::string_view name) {
      std::vector<std::string> parts;
      std::size_t start = 0;
      while (true) {
        const auto colon = name.find(':', start);
        parts.emplace_back(name.substr(start, colon - start));
        if (colon == std::string_view::npos) {
          break;
        }
        start = colon + 1;
      }
      return parts;
    }

  } // namespace

  std::vector<OptionDesc> parseDescriptionsJson(const nlohmann::json& json) {
    std::vector<OptionDesc> out;
    if (!json.is_array()) {
      return out;
    }
    for (const auto& o : json) {
      if (!o.is_object() || !o.contains("name")) {
        continue;
      }
      OptionDesc desc;
      desc.name = o.value("name", "");
      // `hyprctl descriptions` usa guiones (tap-to-click) pero la config Lua solo acepta guion bajo.
      std::ranges::replace(desc.name, '-', '_');
      desc.description = o.value("description", "");
      desc.defaultValue = o.value("default", nlohmann::json());
      desc.current = o.value("current", nlohmann::json());
      if (const auto it = o.find("min"); it != o.end() && it->is_number()) {
        desc.min = it->get<double>();
      }
      if (const auto it = o.find("max"); it != o.end() && it->is_number()) {
        desc.max = it->get<double>();
      }
      if (const auto it = o.find("map"); it != o.end() && it->is_array()) {
        for (const auto& entry : *it) {
          if (entry.is_object() && !entry.empty() && entry.begin()->is_number_integer()) {
            desc.choices.emplace_back(entry.begin().key(), entry.begin()->get<int>());
          }
        }
        std::ranges::sort(desc.choices, {}, &std::pair<std::string, int>::second);
      }
      if (!desc.choices.empty()) {
        desc.kind = OptionKind::Enum;
      } else if (desc.defaultValue.is_boolean()) {
        desc.kind = OptionKind::Bool;
      } else if (desc.defaultValue.is_number_integer()) {
        desc.kind = OptionKind::Int;
      } else if (desc.defaultValue.is_number()) {
        desc.kind = OptionKind::Float;
      } else if (
          desc.defaultValue.is_string()
          && uniformGap(desc.defaultValue).has_value()
          && desc.defaultValue.get<std::string>().find(' ') != std::string::npos
      ) {
        desc.kind = OptionKind::Gaps;
      } else {
        desc.kind = OptionKind::String;
      }
      out.push_back(std::move(desc));
    }
    return out;
  }

  std::vector<OptionDesc> queryOptions(compositors::hyprland::HyprlandRuntime& runtime) {
    const auto json = runtime.requestJson("j/descriptions");
    return json.has_value() ? parseDescriptionsJson(*json) : std::vector<OptionDesc>{};
  }

  bool sameOptionValue(const nlohmann::json& a, const nlohmann::json& b) {
    if (a.is_boolean() || b.is_boolean()) {
      const auto truth = [](const nlohmann::json& v) {
        return v.is_boolean() ? v.get<bool>() : (v.is_number() && v.get<double>() != 0.0);
      };
      return (a.is_boolean() || a.is_number()) && (b.is_boolean() || b.is_number()) && truth(a) == truth(b);
    }
    if (a.is_number() && b.is_number()) {
      return std::abs(a.get<double>() - b.get<double>()) < 1e-4;
    }
    // Gaps: Hyprland reporta "8 8 8 8" y se escribe 8.
    if (const auto ga = uniformGap(a), gb = uniformGap(b); ga.has_value() && gb.has_value()) {
      return *ga == *gb;
    }
    return a == b;
  }

  std::optional<int> uniformGap(const nlohmann::json& value) {
    if (value.is_number_integer()) {
      return value.get<int>();
    }
    if (!value.is_string()) {
      return std::nullopt;
    }
    std::istringstream in(value.get<std::string>());
    std::optional<int> first;
    int n = 0;
    int count = 0;
    while (in >> n) {
      if (first.has_value() && *first != n) {
        return std::nullopt;
      }
      first = n;
      ++count;
    }
    if (!in.eof() || count == 0) {
      return std::nullopt;
    }
    return first;
  }

  std::string serializeOption(std::string_view name, const nlohmann::json& value) {
    const auto parts = splitName(name);
    std::string out = "hl.config({ ";
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
      out += luaKey(parts[i]) + " = { ";
    }
    out += luaKey(parts.back()) + " = " + luaValue(value);
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
      out += " }";
    }
    return out + " })";
  }

  std::string serializeOptionsBlock(const OptionValues& values, const OptionValues& bases) {
    std::string out(kBegin);
    out += " (lo escribe OKARU → Ventanas; va al final para ganar sobre lo anterior)\n";
    for (const auto& [name, value] : values) {
      out += serializeOption(name, value);
      if (const auto base = bases.find(name); base != bases.end()) {
        out += " -- base: " + base->second.dump();
      }
      out += "\n";
    }
    out += kEnd;
    out += "\n";
    return out;
  }

  OptionValues parseOptionsBlock(std::string_view lua, OptionValues* bases) {
    OptionValues values;
    const std::string text(lua);
    const auto begin = text.find(kBegin);
    if (begin == std::string::npos) {
      return values;
    }
    const auto end = text.find(kEnd, begin);
    const std::string block = text.substr(begin, end == std::string::npos ? std::string::npos : end - begin);

    static const std::regex kLine(R"re(^hl\.config\(\{ (.*) \}\)(?: -- base: (.*))?$)re");
    static const std::regex kNest(R"re(^(\w+|\["[^"]+"\]) = \{ (.*) \}$)re");
    static const std::regex kLeaf(R"re(^(\w+|\["[^"]+"\]) = (.*)$)re");
    const auto unkey = [](std::string key) {
      if (key.starts_with("[\"")) {
        key = key.substr(2, key.size() - 4);
      }
      return key;
    };

    std::istringstream in(block);
    std::string line;
    while (std::getline(in, line)) {
      std::smatch m;
      if (!std::regex_match(line, m, kLine)) {
        continue;
      }
      std::string rest = m[1].str();
      const std::string baseText = m[2].matched ? m[2].str() : std::string{};
      std::string name;
      while (std::regex_match(rest, m, kNest)) {
        name += unkey(m[1].str()) + ":";
        rest = m[2].str();
      }
      if (!std::regex_match(rest, m, kLeaf)) {
        continue;
      }
      name += unkey(m[1].str());
      const std::string raw = m[2].str();
      nlohmann::json value;
      if (raw == "true" || raw == "false") {
        value = raw == "true";
      } else if (raw.starts_with('"') && raw.ends_with('"') && raw.size() >= 2) {
        std::string s;
        for (std::size_t i = 1; i + 1 < raw.size(); ++i) {
          if (raw[i] == '\\' && i + 2 < raw.size()) {
            ++i;
          }
          s += raw[i];
        }
        value = s;
      } else if (raw.find_first_of(".eE") != std::string::npos) {
        value = std::strtod(raw.c_str(), nullptr);
      } else {
        value = std::strtoll(raw.c_str(), nullptr, 10);
      }
      values[name] = value;
      if (bases != nullptr && !baseText.empty()) {
        auto base = nlohmann::json::parse(baseText, nullptr, false);
        if (!base.is_discarded()) {
          (*bases)[name] = std::move(base);
        }
      }
    }
    return values;
  }

  std::string replaceTrailingBlock(
      std::string_view fileText, std::string_view beginMarker, std::string_view endMarker, std::string_view block
  ) {
    std::string text(fileText);
    const auto begin = text.find(beginMarker);
    if (begin != std::string::npos) {
      const auto endPos = text.find(endMarker, begin);
      if (endPos != std::string::npos) {
        auto end = endPos + endMarker.size();
        if (end < text.size() && text[end] == '\n') {
          ++end;
        }
        auto start = text.rfind('\n', begin);
        start = start == std::string::npos ? 0 : start + 1;
        if (!block.empty()) {
          return text.replace(start, end - start, block); // en su lugar: el orden de los bloques no cambia
        }
        // Una línea en blanco que separaba el bloque se va con él.
        if (start >= 2 && text[start - 1] == '\n' && text[start - 2] == '\n') {
          --start;
        }
        return text.erase(start, end - start);
      }
    }
    if (block.empty()) {
      return text;
    }
    if (!text.empty() && !text.ends_with('\n')) {
      text += '\n';
    }
    if (!text.empty()) {
      text += '\n';
    }
    return text + std::string(block);
  }

  std::string replaceOptionsBlock(std::string_view fileText, const OptionValues& values, const OptionValues& bases) {
    return replaceTrailingBlock(
        fileText, kBegin, kEnd, values.empty() ? std::string{} : serializeOptionsBlock(values, bases)
    );
  }

  std::filesystem::path okaFilePath(std::string_view file) {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    const std::filesystem::path base = xdg != nullptr && xdg[0] != '\0'
        ? std::filesystem::path(xdg)
        : std::filesystem::path(home != nullptr ? home : "") / ".config";
    return base / "hypr" / "oka" / (std::string(file) + ".lua");
  }

  OptionValues readOptionOverrides(const std::filesystem::path& path, OptionValues* bases) {
    return parseOptionsBlock(readOkaFile(path), bases);
  }

  std::string readOkaFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      return {};
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
  }

  std::optional<std::string>
  writeOptionOverrides(const std::filesystem::path& path, const OptionValues& values, const OptionValues& bases) {
    const std::string current = readOkaFile(path);
    const std::string next = replaceOptionsBlock(current, values, bases);
    if (next == current) {
      return std::nullopt;
    }
    return writeFileAtomic(path, next);
  }

  std::optional<std::string> writeFileAtomic(const std::filesystem::path& path, std::string_view next) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const auto tmp = path.string() + ".okaru-tmp";
    {
      std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
      if (!out || !(out << next) || !out.flush()) {
        return "no se pudo escribir " + tmp;
      }
    }
    std::filesystem::rename(tmp, path, ec); // atómico: Hyprland nunca lee un archivo a medias
    if (ec) {
      std::filesystem::remove(tmp, ec);
      return "no se pudo reemplazar " + path.string();
    }
    return std::nullopt;
  }

  std::optional<std::string>
  applyOption(compositors::hyprland::HyprlandRuntime& runtime, std::string_view name, const nlohmann::json& value) {
    const auto response = runtime.request("eval " + serializeOption(name, value));
    if (!response.has_value()) {
      return "Hyprland no respondió";
    }
    if (response->empty() || response->starts_with("ok")) {
      return std::nullopt;
    }
    return *response;
  }

} // namespace okaru::hypr
