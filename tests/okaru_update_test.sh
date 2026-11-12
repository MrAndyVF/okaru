#!/usr/bin/env bash
# okaru-update con comandos falsos en PATH: --check por canal, --ignore y los comandos que arma cada modo.
set -euo pipefail

script=$(realpath "${1:-data/bin/okaru-update}")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
bin=$tmp/bin calls=$tmp/calls
mkdir -p "$bin"

fake() { # $1 = comando, $2 = salida
  printf '#!/usr/bin/env bash\necho "%s $*" >>"%s"\nprintf "%%b" %q\n' "$1" "$calls" "$2" >"$bin/$1"
  chmod +x "$bin/$1"
}
fake checkupdates 'linux 6.1 -> 6.2\nmesa 1 -> 2\n'
fake yay 'foo-git r1 -> r2\n'
fake flatpak 'org.app.A\norg.app.B\n'
fake snap 'Name Version Rev Size Publisher Notes\nintellij-idea 2026.3 155 1GB jetbrains classic\n'
fake sudo ''
fake pkexec ''
# Solo los falsos y lo básico del sistema: así paru real no se cuela.
for c in bash awk cat env mapfile; do p=$(command -v "$c" 2>/dev/null) && ln -sf "$p" "$bin/$c"; done
run() { : >"$calls"; PATH=$bin "$bin/bash" "$script" "$@"; }

fail() { echo "FALLO: $*" >&2; exit 1; }

out=$(run --check --ignore mesa,org.app.B)
grep -q '^linux 6.1 -> 6.2$' <<<"$out" || fail "check: falta linux"
grep -q 'mesa' <<<"$out" && fail "check: mesa debía ignorarse"
grep -q '^foo-git' <<<"$out" || fail "check: falta AUR"
grep -q '^org.app.A$' <<<"$out" || fail "check: falta Flatpak"
grep -q 'org.app.B' <<<"$out" && fail "check: org.app.B debía ignorarse"
grep -q '^intellij-idea$' <<<"$out" || fail "check: falta Snap"
grep -q '^Name$' <<<"$out" && fail "check: el encabezado de snap no es un paquete"

run --check --no-flatpak --no-snap --no-aur >/dev/null
grep -qE '^(yay|flatpak|snap) ' "$calls" && fail "check: --no-* no se respetó"

run >/dev/null
grep -qx 'yay -Syu' "$calls" || fail "terminal: yay -Syu"
grep -qx 'flatpak update -y --noninteractive' "$calls" || fail "terminal: flatpak update"
grep -qx 'sudo snap refresh' "$calls" || fail "terminal: sudo snap refresh"

run -y --ignore mesa,intellij-idea >/dev/null
grep -q '^yay -Syu --sudo pkexec .*--noconfirm.* --ignore mesa,intellij-idea$' "$calls" || fail "fondo: yay"
grep -qx 'flatpak update -y --noninteractive org.app.A org.app.B' "$calls" || fail "fondo: flatpak filtrado"
grep -q 'snap refresh$' "$calls" && fail "fondo: snap ignorado no debía refrescarse"

run -y --no-aur --no-snap >/dev/null
grep -q '^pkexec pacman -Syu --noconfirm' "$calls" || fail "fondo: pkexec pacman"

# Un paso que falla no detiene a los siguientes y su código se conserva.
printf '#!/usr/bin/env bash\necho "yay $*" >>"%s"\nexit 3\n' "$calls" >"$bin/yay"
set +e; run >/dev/null; code=$?; set -e
[[ $code == 3 ]] || fail "código de salida $code, se esperaba 3"
grep -q '^snap ' "$calls" || grep -q 'snap refresh' "$calls" || fail "snap no corrió tras fallar yay"

echo "okaru-update: OK"
