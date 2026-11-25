#!/usr/bin/env bash
# OKARU: CI de las apps. Reemplaza el okaru-core publicado (ya instalado por pacman) por el de este checkout de
# okaru (una rama o commit sin publicar), para probar una app que estrena API del núcleo antes de la release.
# Uso, desde la raíz de un clon de okaru:  bash packaging/ci/instalar-nucleo.sh
# Solo compila las dos bibliotecas (no la shell) y copia lo mismo que trae el paquete okaru-core.
set -euo pipefail

src=$(pwd)
build=/tmp/okaru-core-build # fija: con una carpeta al azar ccache no acertaría entre ejecuciones
rm -rf "$build"

# shellcheck disable=SC1091
source packaging/arch/PKGBUILD
pacman -S --noconfirm --needed "${depends[@]}" "${makedepends[@]}" >/dev/null

meson setup "$build" "$src" --prefix=/usr --buildtype=release -Dtests=disabled >/dev/null
meson compile -C "$build" okaru-core okaru-apps

install -Dm755 "$build/libokaru-core.so" /usr/lib/okaru/libokaru-core.so
install -Dm755 "$build/libokaru-apps.so" /usr/lib/okaru/libokaru-apps.so
# Cabeceras: las de src/ con su ruta (sin «src/») y las de los protocolos generadas al compilar.
rm -rf /usr/include/okaru
(cd src && find . -name '*.h' -exec install -Dm644 {} /usr/include/okaru/{} \;)
find "$build" -maxdepth 1 -name '*-client-protocol.h' -exec install -Dm644 -t /usr/include/okaru {} +
rm -rf /usr/share/okaru/assets
cp -r assets /usr/share/okaru/assets

echo "okaru-core reemplazado por $(git rev-parse --short HEAD) ($(git describe --tags --always))"
