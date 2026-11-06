#!/usr/bin/env -S bash
# Imprime las notas de una versión de CHANGELOG.md (sin su título "## [x.y.z]"); falla si no existen.
# Uso: tools/changelog-section.sh 1.0.1 [CHANGELOG.md]
set -euo pipefail
version="${1#v}"
notes=$(awk -v head="## [$version]" '/^## \[/ { p = index($0, head) == 1; next } p' "${2:-CHANGELOG.md}")
if [ -z "${notes//[[:space:]]/}" ]; then
  echo "CHANGELOG.md no tiene notas para la versión $version" >&2
  exit 1
fi
printf '%s\n' "$notes" | sed '/./,$!d'
