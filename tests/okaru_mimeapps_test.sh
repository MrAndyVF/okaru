#!/usr/bin/env bash
# OKARU: la lista de apps por defecto asocia a cada app exactamente los tipos que declara su .desktop
# (las imágenes al visor; todo el MimeType al editor, al lector PDF, al reproductor, al explorador y a Comprimidos).
set -euo pipefail
list=packaging/defaults/okaru-mimeapps.list

check() { # $1 = .desktop, $2 = filtro de tipos (regex)
  local desktop=assets/$1 declared mapped
  declared=$(grep '^MimeType=' "$desktop" | cut -d= -f2 | tr ';' '\n' | grep -E "$2" | sort)
  mapped=$(grep "=${1//./\\.}\$" "$list" | cut -d= -f1 | sort)
  if [[ "$declared" != "$mapped" ]]; then
    echo "okaru_mimeapps_test: $desktop y $list no coinciden:" >&2
    diff <(echo "$declared") <(echo "$mapped") >&2 || true
    exit 1
  fi
}
check dev.okaru.Viewer.desktop '^image/'
check dev.okaru.Editor.desktop '.'
check dev.okaru.Pdf.desktop '.'
check dev.okaru.Player.desktop '.'
check dev.okaru.Files.desktop '.'
check dev.okaru.Archive.desktop '.'
grep -q '^\[Default Applications\]$' "$list" || { echo "okaru_mimeapps_test: falta [Default Applications]" >&2; exit 1; }
