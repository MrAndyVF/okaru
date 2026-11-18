#!/usr/bin/env bash

# Con la configuración propia conservada, el instalador agrega las reglas de ventana de las apps de OKARU en un
# archivo aparte que se carga desde la suya, una sola vez, y no toca un hyprland.conf del formato viejo.
set -euo pipefail

installer=${1:?install.sh}
defaults=${2:?packaging/defaults}

fail() {
  printf '%s\n' "install_app_rules_test: FAIL: $*" >&2
  exit 1
}

work_dir=$(mktemp -d)
trap 'rm -rf "$work_dir"' EXIT

eval "$(sed -n '/^app_rules() {/,/^}/p' "$installer")"
config_target() { echo "$work_dir/$COMPOSITOR"; }
defaults_dir() { echo "$defaults/$([[ $COMPOSITOR == niri ]] && echo niri || echo hypr)"; }

# Hyprland con hyprland.lua propio.
COMPOSITOR=hyprland
mkdir -p "$work_dir/hyprland"
printf 'require("mio")\n' >"$work_dir/hyprland/hyprland.lua"
app_rules
app_rules # la segunda vez ya las encuentra
[[ $(grep -c 'okaru-apps' "$work_dir/hyprland/hyprland.lua") == 1 ]] || fail "hyprland.lua no carga okaru-apps una sola vez"
for app in Calculator Viewer Player Editor FilePicker Files Archive Pdf Music; do
  grep -qF "dev\\\\.okaru\\\\.$app\$" "$work_dir/hyprland/okaru-apps.lua" || fail "falta la regla de $app en Hyprland"
done
grep -q '^hl.on("window.active"' "$work_dir/hyprland/okaru-apps.lua" || fail "falta la flotante encima de la maximizada"
[[ $(tail -1 "$work_dir/hyprland/okaru-apps.lua") == 'end)' ]] || fail "el bloque window.active quedó cortado"

# Hyprland con hyprland.conf (formato viejo): no se toca.
COMPOSITOR=hyprland
rm -rf "$work_dir/hyprland" && mkdir -p "$work_dir/hyprland"
printf 'monitor = ,preferred,auto,1\n' >"$work_dir/hyprland/hyprland.conf"
app_rules
[[ ! -e $work_dir/hyprland/okaru-apps.lua && $(cat "$work_dir/hyprland/hyprland.conf") == 'monitor = ,preferred,auto,1' ]] ||
  fail "tocó un hyprland.conf"

# Niri.
COMPOSITOR=niri
mkdir -p "$work_dir/niri"
printf 'input {\n}\n' >"$work_dir/niri/config.kdl"
app_rules
app_rules
[[ $(grep -c 'include optional=true "okaru/apps.kdl"' "$work_dir/niri/config.kdl") == 1 ]] || fail "config.kdl no incluye apps.kdl una sola vez"
for app in Calculator Viewer Player Editor FilePicker Files Archive Pdf Music; do
  grep -qF "dev\\\\.okaru\\\\.$app\$" "$work_dir/niri/okaru/apps.kdl" || fail "falta la regla de $app en Niri"
done
[[ $(grep -c '^window-rule {' "$work_dir/niri/okaru/apps.kdl") == $(grep -c '^}' "$work_dir/niri/okaru/apps.kdl") ]] ||
  fail "bloques window-rule sin cerrar en apps.kdl"
grep -q 'steam' "$work_dir/niri/okaru/apps.kdl" && fail "copió reglas que no son de OKARU"
exit 0
