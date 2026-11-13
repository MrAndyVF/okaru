# OKARU (郁)

**Español** · [English](#english)

Shell de escritorio nativa para Wayland (Hyprland y Niri): barra, dock, lanzador, centro de control,
notificaciones, fondo de pantalla, pantalla de bloqueo, OSD, portapapeles y ajustes con gestión de
Hyprland, todo en un solo programa sobre Wayland y OpenGL ES, sin Qt ni GTK.

**Soporte:** solo Arch Linux y CachyOS.

## Instalar

```sh
curl -fsSL https://raw.githubusercontent.com/MrAndyVF/okaru/main/install.sh | bash
```

Agrega el repositorio pacman `[okaru]` (paquetes firmados, se actualizan con `pacman -Syu`), te deja elegir
el compositor (Hyprland con uwsm, o Niri) e instala la shell con él, kitty y okaru-files como gestor de archivos, y deja su
configuración de fábrica (reglas y atajos estándar; en Hyprland tus atajos propios van en
`~/.config/hypr/oka/user-binds.lua`, en Niri en `~/.config/niri/config.kdl`). Pregunta antes de reemplazar
una configuración existente y si quieres el greeter, el teléfono (KDE Connect) y las dependencias opcionales.

## Compilar

```sh
meson setup build-debug && meson compile -C build-debug okaru
meson test -C build-debug
```

Dependencias y opciones en [BUILDING.md](BUILDING.md). Paquete local de Arch: `packaging/arch/PKGBUILD`
(compila los commits de la rama `main`).

## Ecosistema

- [okaru-greeter](https://github.com/MrAndyVF/okaru-greeter): pantalla de inicio de sesión para greetd.
- [okaru-plugins](https://github.com/MrAndyVF/okaru-plugins): catálogo de plugins.
- [okaru-templates](https://github.com/MrAndyVF/okaru-templates) y
  [okaru-palettes](https://github.com/MrAndyVF/okaru-palettes): plantillas y paletas.

## Atribución

OKARU es un proyecto independiente creado y mantenido por MrAndyVF. Parte de su código base proviene de
[Noctalia](https://github.com/noctalia-dev/noctalia) (licencia MIT). Gracias al equipo y a la comunidad de
Noctalia; sus créditos están en [CREDITS.md](CREDITS.md) y su aviso de copyright se conserva en
[LICENSE](LICENSE).

---

## English

Native desktop shell for Wayland (Hyprland and Niri): bar, dock, launcher, control center,
notifications, wallpaper, lock screen, OSD, clipboard and settings with Hyprland management, all in a
single program on Wayland and OpenGL ES, without Qt or GTK.

**Support:** Arch Linux and CachyOS only.

### Install

```sh
curl -fsSL https://raw.githubusercontent.com/MrAndyVF/okaru/main/install.sh | bash
```

Adds the signed `[okaru]` pacman repository, lets you pick the compositor (Hyprland with uwsm, or Niri),
installs the shell with it, kitty and okaru-files as file manager, and sets up its stock config (standard rules and keybindings;
on Hyprland your own go in `~/.config/hypr/oka/user-binds.lua`, on Niri in `~/.config/niri/config.kdl`).

### Build

```sh
meson setup build-debug && meson compile -C build-debug okaru
meson test -C build-debug
```

Dependencies and options in [BUILDING.md](BUILDING.md). Local Arch package: `packaging/arch/PKGBUILD`
(builds the commits on the `main` branch).

### Ecosystem

- [okaru-greeter](https://github.com/MrAndyVF/okaru-greeter): login screen for greetd.
- [okaru-plugins](https://github.com/MrAndyVF/okaru-plugins): plugin catalog.
- [okaru-templates](https://github.com/MrAndyVF/okaru-templates) and
  [okaru-palettes](https://github.com/MrAndyVF/okaru-palettes): templates and palettes.

### Attribution

OKARU is an independent project created and maintained by MrAndyVF. Part of its code base comes from
[Noctalia](https://github.com/noctalia-dev/noctalia) (MIT license). Thanks to the Noctalia team and
community; their credits are in [CREDITS.md](CREDITS.md) and their copyright notice is kept in
[LICENSE](LICENSE).
