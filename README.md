# OKARU (郁)

**Español** · [English](#english)

Shell de escritorio nativa para Wayland (Hyprland y Niri): barra, dock, lanzador, centro de control,
notificaciones, fondo de pantalla, pantalla de bloqueo, OSD, portapapeles y ajustes con gestión de
Hyprland, todo en un solo programa sobre Wayland y OpenGL ES, sin Qt ni GTK.

**Soporte:** solo Arch Linux y CachyOS.

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
