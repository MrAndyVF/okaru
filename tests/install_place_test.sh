#!/usr/bin/env bash

# El instalador copia la configuración de fábrica si falta, conserva la del usuario si así lo eligió y, al
# reemplazar, deja una copia fechada. Sin teléfono, quita el widget solo de la barra de fábrica.
set -euo pipefail

installer=${1:?install.sh}
factory=${2:?config.toml de fábrica}

fail() {
  printf '%s\n' "install_place_test: FAIL: $*" >&2
  exit 1
}

work_dir=$(mktemp -d)
trap 'rm -rf "$work_dir"' EXIT

t() { printf '%s' "$2"; }
eval "$(sed -n '/^backup() {/,/^}/p;/^place() {/,/^}/p' "$installer")"
STAMP=test

printf 'fábrica\n' >"$work_dir/src"
CONFIG_ACTION=keep
place "$work_dir/src" "$work_dir/new/dest" >/dev/null
[[ $(cat "$work_dir/new/dest") == fábrica ]] || fail "no copió lo que faltaba"

printf 'mía\n' >"$work_dir/mine"
place "$work_dir/src" "$work_dir/mine" >/dev/null
[[ $(cat "$work_dir/mine") == mía ]] || fail "keep pisó la configuración propia"

CONFIG_ACTION=replace
place "$work_dir/src" "$work_dir/mine" >/dev/null
[[ $(cat "$work_dir/mine") == fábrica ]] || fail "replace no copió la de fábrica"
[[ $(cat "$work_dir/mine.bak-test") == mía ]] || fail "replace no dejó la copia de seguridad"

phone_sed=$(grep -o "sed -i 's/^end = .*/' " "$installer") || fail "no se encontró el sed del teléfono"
cp "$factory" "$work_dir/config.toml"
eval "${phone_sed}\"\$work_dir/config.toml\""
grep -q '^end = \[ "tray", "cat", "input_volume",' "$work_dir/config.toml" || fail "el teléfono sigue en la barra de fábrica"
