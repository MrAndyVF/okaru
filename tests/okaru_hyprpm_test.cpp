// OKARU: estado y comandos de hyprpm.
#include "compositors/hyprland/okaru_hyprpm.h"

#include <cstdlib>
#include <print>

using namespace okaru::hyprpm;

namespace {

  bool expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "okaru_hyprpm_test: {}", message);
    }
    return condition;
  }

  // Igual que /var/cache/hyprpm/andress/hyprland-scroll-overview/state.toml el 2026-11-01.
  constexpr const char* kState = R"([repository]
author = 'MrAndyVF'
hash = 'db3c4daebb518b9b8c265b9646d5dd0680795a42'
name = 'hyprland-scroll-overview'
rev = ''
url = 'https://github.com/MrAndyVF/hyprland-scroll-overview'

[scrolloverview]
enabled = true
failed = false
filename = 'scrolloverview.so'
)";

} // namespace

int main() {
  bool ok = true;

  const auto repo = parseRepoState(kState);
  ok = expect(repo.has_value() && repo->name == "hyprland-scroll-overview" && repo->author == "MrAndyVF", "repo") && ok;
  ok = expect(repo->plugins.size() == 1 && repo->plugins[0].name == "scrolloverview", "un plugin") && ok;
  ok = expect(repo->plugins[0].enabled && !repo->plugins[0].failed, "activo y sin fallo") && ok;
  ok = expect(!parseRepoState("[otra]\nx = 1\n").has_value(), "sin [repository] no es repo") && ok;
  ok = expect(!parseRepoState("no es toml [").has_value(), "toml roto") && ok;

  const std::vector<Repo> repos{*repo};
  ok = expect(installed(repos, "https://github.com/mrandyvf/hyprland-scroll-overview.git/"), "url normalizada") && ok;
  ok = expect(!installed(repos, "https://github.com/hyprwm/hyprland-plugins"), "no instalado") && ok;

  // Nada que llegue a `sh -c` puede romper la línea.
  ok = expect(!commandFor(Action::Add, "https://x.com/a;rm -rf ~").has_value(), "url con ; rechazada") && ok;
  ok = expect(!commandFor(Action::Add, "https://x.com/a'b").has_value(), "url con comilla rechazada") && ok;
  ok = expect(!commandFor(Action::Add, "file:///etc").has_value(), "esquema no git rechazado") && ok;
  ok = expect(!commandFor(Action::Enable, "-rf").has_value(), "nombre que parece opción") && ok;
  ok = expect(!commandFor(Action::Remove, "a b").has_value(), "nombre con espacio") && ok;
  const auto add = commandFor(Action::Add, "https://github.com/hyprwm/hyprland-plugins");
  ok = expect(
           add.has_value()
               && add->at(2) == "yes | hyprpm add https://github.com/hyprwm/hyprland-plugins && hyprpm reload",
           "comando add"
       )
      && ok;
  ok = expect(commandFor(Action::Update, "").value().at(2) == "yes | hyprpm update && hyprpm reload", "update") && ok;

  ok = expect(cleanOutput("\x1b[32mok\x1b[0m\nbar 10%\rbar 100%\n") == "ok\nbar 100%\n", "limpia ANSI y \\r") && ok;

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
