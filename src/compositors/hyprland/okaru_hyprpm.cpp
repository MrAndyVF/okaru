#include "compositors/hyprland/okaru_hyprpm.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <pwd.h>
#include <sstream>
#include <toml++/toml.hpp>
#include <unistd.h>

namespace okaru::hyprpm {

  namespace {

    constexpr std::array<Suggestion, 2> kSuggestions{{
        {"hyprland-plugins", "hyprwm", "https://github.com/hyprwm/hyprland-plugins", "suggestion-hyprland-plugins"},
        {"hyprland-scroll-overview", "MrAndyVF", "https://github.com/MrAndyVF/hyprland-scroll-overview",
         "suggestion-scroll-overview"},
    }};

    std::string normalizedUrl(std::string_view url) {
      std::string out(url);
      while (out.ends_with('/')) {
        out.pop_back();
      }
      if (out.ends_with(".git")) {
        out.resize(out.size() - 4);
      }
      std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return out;
    }

    std::string userName() {
      if (const passwd* pw = getpwuid(getuid()); pw != nullptr && pw->pw_name != nullptr) {
        return pw->pw_name;
      }
      const char* user = std::getenv("USER");
      return user != nullptr ? user : "";
    }

  } // namespace

  std::optional<Repo> parseRepoState(std::string_view text) {
    toml::table table;
    try {
      table = toml::parse(text);
    } catch (const toml::parse_error&) {
      return std::nullopt;
    }
    const auto* repo = table["repository"].as_table();
    if (repo == nullptr) {
      return std::nullopt;
    }
    Repo out{
        .name = (*repo)["name"].value_or(std::string()),
        .author = (*repo)["author"].value_or(std::string()),
        .url = (*repo)["url"].value_or(std::string()),
        .hash = (*repo)["hash"].value_or(std::string()),
        .plugins = {},
    };
    if (out.name.empty()) {
      return std::nullopt;
    }
    for (const auto& [key, node] : table) {
      const auto* plugin = node.as_table();
      if (key.str() == "repository" || plugin == nullptr) {
        continue;
      }
      out.plugins.push_back({
          .name = std::string(key.str()),
          .enabled = (*plugin)["enabled"].value_or(false),
          .failed = (*plugin)["failed"].value_or(false),
      });
    }
    std::ranges::sort(out.plugins, {}, &Plugin::name);
    return out;
  }

  std::filesystem::path stateRoot() { return std::filesystem::path("/var/cache/hyprpm") / userName(); }

  std::vector<Repo> readRepos() {
    std::vector<Repo> repos;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(stateRoot(), ec)) {
      if (!entry.is_directory(ec)) {
        continue;
      }
      std::ifstream file(entry.path() / "state.toml");
      if (!file) {
        continue;
      }
      std::stringstream buffer;
      buffer << file.rdbuf();
      if (auto repo = parseRepoState(buffer.str())) {
        repos.push_back(std::move(*repo));
      }
    }
    std::ranges::sort(repos, {}, &Repo::name);
    return repos;
  }

  std::span<const Suggestion> suggestions() { return kSuggestions; }

  bool installed(const std::vector<Repo>& repos, std::string_view url) {
    const std::string wanted = normalizedUrl(url);
    return std::ranges::any_of(repos, [&](const Repo& r) { return normalizedUrl(r.url) == wanted; });
  }

  bool isSafeGitUrl(std::string_view url) {
    if (!(url.starts_with("https://") || url.starts_with("http://") || url.starts_with("git@"))) {
      return false;
    }
    return url.size() < 512
        && std::ranges::all_of(
               url,
               [](unsigned char c) {
                 return std::isalnum(c) != 0
                     || std::string_view(":/._-@~+%").find(static_cast<char>(c)) != std::string_view::npos;
               }
        );
  }

  bool isSafeName(std::string_view name) {
    return !name.empty()
        && name.size() < 128
        && name.front() != '-'
        && std::ranges::all_of(
            name, [](unsigned char c) { return std::isalnum(c) != 0 || c == '_' || c == '-' || c == '.' || c == '/'; }
        );
  }

  std::optional<std::vector<std::string>> commandFor(Action action, std::string_view arg) {
    std::string verb;
    switch (action) {
    case Action::Add:
      if (!isSafeGitUrl(arg)) {
        return std::nullopt;
      }
      verb = "add";
      break;
    case Action::Remove:
      verb = "remove";
      break;
    case Action::Enable:
      verb = "enable";
      break;
    case Action::Disable:
      verb = "disable";
      break;
    case Action::Update:
      verb = "update";
      break;
    }
    std::string line = "yes | hyprpm " + verb;
    if (action != Action::Update) {
      if (action != Action::Add && !isSafeName(arg)) {
        return std::nullopt;
      }
      line += " " + std::string(arg);
    }
    // `yes` responde las confirmaciones de hyprpm; reload carga o descarga lo que cambió.
    line += " && hyprpm reload";
    return std::vector<std::string>{"sh", "-c", line};
  }

  std::vector<process::EnvOverride> authEnv(std::string message) {
    const char* path = std::getenv("PATH");
    return {
        {.name = "PATH", .value = "/usr/lib/okaru/shim:" + std::string(path != nullptr ? path : "/usr/bin")},
        {.name = "SUDO_ASKPASS", .value = "/usr/lib/okaru/okaru-askpass"},
        {.name = "OKARU_AUTH_MESSAGE", .value = std::move(message)},
        {.name = "NO_COLOR", .value = "1"},
    };
  }

  std::string cleanOutput(std::string_view raw) {
    std::string out;
    std::string line;
    const auto flush = [&] {
      out += line;
      out += '\n';
      line.clear();
    };
    for (std::size_t i = 0; i < raw.size(); ++i) {
      const char c = raw[i];
      if (c == '\x1b' && i + 1 < raw.size() && raw[i + 1] == '[') {
        i += 2;
        while (i < raw.size() && (std::isalpha(static_cast<unsigned char>(raw[i])) == 0)) {
          ++i;
        }
        continue;
      }
      if (c == '\r') {
        line.clear();
      } else if (c == '\n') {
        flush();
      } else {
        line += c;
      }
    }
    if (!line.empty()) {
      flush();
    }
    return out;
  }

} // namespace okaru::hyprpm
