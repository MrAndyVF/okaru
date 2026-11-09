#!/usr/bin/env bash
# OKARU: la lista de apps por defecto asocia al visor exactamente las imágenes que declara su .desktop.
set -euo pipefail
desktop=assets/dev.okaru.Viewer.desktop
list=packaging/defaults/okaru-mimeapps.list

declared=$(grep '^MimeType=' "$desktop" | cut -d= -f2 | tr ';' '\n' | grep '^image/' | sort)
mapped=$(grep '=dev\.okaru\.Viewer\.desktop$' "$list" | cut -d= -f1 | sort)

if [[ "$declared" != "$mapped" ]]; then
  echo "okaru_mimeapps_test: el .desktop y $list no coinciden:" >&2
  diff <(echo "$declared") <(echo "$mapped") >&2 || true
  exit 1
fi
grep -q '^\[Default Applications\]$' "$list" || { echo "okaru_mimeapps_test: falta [Default Applications]" >&2; exit 1; }
