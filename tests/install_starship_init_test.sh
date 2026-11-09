#!/usr/bin/env bash

# El instalador debe dejar `starship init` al final del archivo del shell (después del source de CachyOS)
# y no repetirlo si ya está.
set -euo pipefail

installer=${1:?install.sh}

fail() {
  printf '%s\n' "install_starship_init_test: FAIL: $*" >&2
  exit 1
}

work_dir=$(mktemp -d)
trap 'rm -rf "$work_dir"' EXIT

ES=false
t() { printf '%s' "$2"; }
eval "$(sed -n '/^starship_init() {/,/^}/p' "$installer")"

home="$work_dir/home" config="$work_dir/home/.config"
mkdir -p "$config/fish"
printf 'source /usr/share/cachyos-fish-config/cachyos-config.fish\n' >"$config/fish/config.fish"
printf 'source /usr/share/cachyos-zsh-config/cachyos-config.zsh\n' >"$home/.zshrc"

for shell in /usr/bin/fish /bin/zsh bash; do
  starship_init "$shell" "$config" "$home" "$home" >/dev/null
  starship_init "$shell" "$config" "$home" "$home" >/dev/null
done

[[ $(tail -n1 "$config/fish/config.fish") == *'starship init fish'* ]] || fail "fish: init no quedó al final"
[[ $(grep -c 'starship init' "$config/fish/config.fish") == 1 ]] || fail "fish: init repetido"
grep -q 'powerlevel10k_plugin_unload' "$home/.zshrc" || fail "zsh: no descarga powerlevel10k"
[[ $(tail -n1 "$home/.zshrc") == *'starship init zsh'* ]] || fail "zsh: init no quedó al final"
[[ $(grep -c 'starship init' "$home/.zshrc") == 1 ]] || fail "zsh: init repetido"
[[ $(grep -c 'starship init bash' "$home/.bashrc") == 1 ]] || fail "bash: init ausente o repetido"
starship_init /usr/bin/nu "$config" "$home" "$home" | grep -q 'unsupported' || fail "shell sin soporte no avisa"

if command -v zsh >/dev/null; then zsh -n "$home/.zshrc" || fail "zsh: sintaxis inválida"; fi
command -v fish >/dev/null && { fish -n "$config/fish/config.fish" || fail "fish: sintaxis inválida"; }
printf '%s\n' "install_starship_init_test: ok"
