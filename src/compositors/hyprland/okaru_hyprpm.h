#pragma once

// OKARU: plugins de Hyprland con hyprpm. Lee el estado de /var/cache/hyprpm/<usuario> (legible sin sudo)
// y arma los comandos; las acciones piden la contraseña con el diálogo de OKARU (sudo -A + okaru askpass).

#include "core/process/process.h"

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace okaru::hyprpm {

  struct Plugin {
    std::string name;
    bool enabled = false;
    bool failed = false;
  };

  struct Repo {
    std::string name;
    std::string author;
    std::string url;
    std::string hash;
    std::vector<Plugin> plugins;
  };

  // Repos que se ofrecen en la página. Solo se ofrecen: nada se agrega solo.
  struct Suggestion {
    std::string_view name;
    std::string_view author;
    std::string_view url;
    std::string_view descriptionKey; // settings.okaru.hyprpm.suggestion-<...>
  };

  enum class Action { Add, Remove, Enable, Disable, Update };

  [[nodiscard]] std::optional<Repo> parseRepoState(std::string_view text);
  [[nodiscard]] std::filesystem::path stateRoot();
  [[nodiscard]] std::vector<Repo> readRepos();
  [[nodiscard]] std::span<const Suggestion> suggestions();
  [[nodiscard]] bool installed(const std::vector<Repo>& repos, std::string_view url);

  // Validación de lo que llega a la línea de comandos (va dentro de `sh -c`).
  [[nodiscard]] bool isSafeGitUrl(std::string_view url);
  [[nodiscard]] bool isSafeName(std::string_view name);

  // argv completo, o nullopt si el argumento no es seguro. Tras cambiar algo se recarga (`hyprpm reload`).
  [[nodiscard]] std::optional<std::vector<std::string>> commandFor(Action action, std::string_view arg);
  // Entorno para que sudo pida la contraseña con el diálogo de OKARU; `message` es el contexto del diálogo.
  [[nodiscard]] std::vector<process::EnvOverride> authEnv(std::string message);

  // Quita colores ANSI y deja la última reescritura de cada línea (las barras de progreso usan \r).
  [[nodiscard]] std::string cleanOutput(std::string_view raw);

} // namespace okaru::hyprpm
