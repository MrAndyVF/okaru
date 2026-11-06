# Changelog

Each version has an English and a Spanish section, grouped by type of change:
**Features**, **Fixes**, **Regressions** (something that worked before and broke), **Changes** (behaviour,
packaging or internals). Empty groups are left out. The publish workflow copies the section of the tagged
version into its GitHub release, and Settings → Home links to it.

Cada versión tiene una sección en inglés y otra en español, separadas por tipo de cambio:
**Novedades**, **Correcciones**, **Regresiones** (algo que funcionaba y se rompió), **Cambios**
(comportamiento, empaquetado o funcionamiento interno). Los grupos vacíos se omiten. El flujo de publicación
copia la sección de la versión etiquetada en su release de GitHub, y Ajustes → Inicio enlaza a él.

## [1.0.2] - 2026-11-06

### English

#### Features
- The installer lets you choose the compositor, Hyprland (with uwsm) or Niri, and installs the basics for
  each one (Niri: `xdg-desktop-portal-gnome` and `xwayland-satellite`).
- Stock Niri config with OKARU's keybindings and window rules, copied to `~/.config/niri` when Niri is
  chosen.
- On Hyprland, the default bar includes the layout widget.
- Settings → Home → What's new links to the changelog of the latest release.
- Each version now has its own GitHub release with this changelog in English and Spanish.

#### Fixes
- Publishing from a `v*` tag failed with "invalid reference: origin/main".
- The new layout keybindings had Spanish descriptions in the stock config.
- Error messages shown in Settings (Hyprland, startup apps) were in Spanish with the interface in English.
- The keybindings page shows the Layouts, "Focus and move", Audio and Apps sections with translated names.

#### Regressions
- Since 1.0.1, the focus/move and per-layout keybindings were listed under Other instead of Windows.

### Español

#### Novedades
- El instalador deja elegir el compositor, Hyprland (con uwsm) o Niri, e instala lo básico de cada uno
  (Niri: `xdg-desktop-portal-gnome` y `xwayland-satellite`).
- Configuración de fábrica de Niri con los atajos y reglas de ventana de OKARU, copiada a `~/.config/niri`
  al elegir Niri.
- En Hyprland, la barra por defecto incluye el widget de layout.
- Ajustes → Inicio → Novedades enlaza al changelog de la última release.
- Cada versión tiene su propio release en GitHub con este changelog en inglés y en español.

#### Correcciones
- Publicar desde una etiqueta `v*` fallaba con "invalid reference: origin/main".
- Los atajos nuevos de layouts tenían la descripción en español en la configuración de fábrica.
- Los mensajes de error de Ajustes (Hyprland, aplicaciones de inicio) salían en español con la interfaz en
  inglés.
- La página de atajos muestra traducidas las secciones Layouts, "Foco y mover", Audio y Aplicaciones.

#### Regresiones
- Desde la 1.0.1, los atajos de foco/mover y los que dependen del layout aparecían en Otros en lugar de
  Ventanas.

## [1.0.1] - 2026-11-06

### English

#### Features
- Hyprland layouts (scrolling, dwindle, master, monocle): `Super+Alt+Space` / `Super+Alt+Shift+Space`
  cycles them, with an OSD and a new `hypr_layout` bar widget.
- Setting in Windows → Layout to change the layout of the current workspace only or of the whole
  system; per-workspace layouts survive restarts.
- Layout-aware keybindings: `Super+R`, `Super+Shift+R`, `Super+=`/`-`, `Super+Ctrl+Return`,
  `Super+Alt+=`/`-` and `Super+P`.
- Native phone integration through KDE Connect: bar widget, panel, control center card, automatic
  clipboard and recent images (replaces the Luau phone-connect module).
- VPN credentials are asked in OKARU's own dialog, without a terminal.
- Sound panel with per-application volume and system sounds; devices update live in Settings.
- OKARU's own palette (muted blues over greys and black) and identity: 郁 wallpaper, kanji logo.
- Installer through `curl` and a signed pacman repository `[okaru]`.
- OKARU's polkit agent is enabled by default.

#### Fixes
- Scrolling layout: focus and swap with `Super+arrows` now work while a window is maximized or after
  turning a floating window into a tiled one.
- On a clean install, workspaces start at 1 and the built-in modules are enabled.
- Missing dependencies (Noto fonts, accountsservice with the greeter, build dependencies).
- The installer detects the session manager by its real alias.

#### Changes
- The last references to noctalia.dev were removed.
- Optional dependencies of the built-in modules are declared in the package.
- Required CI checks on `main`; package versions come only from `v*` tags.

### Español

#### Novedades
- Layouts de Hyprland (scrolling, dwindle, master, monocle): `Super+Alt+Space` / `Super+Alt+Shift+Space`
  los recorre, con OSD y un nuevo widget de barra `hypr_layout`.
- Opción en Ventanas → Distribución para cambiar el layout solo del workspace actual o de todo el sistema;
  los layouts por workspace se conservan tras reiniciar.
- Atajos según el layout: `Super+R`, `Super+Shift+R`, `Super+=`/`-`, `Super+Ctrl+Return`,
  `Super+Alt+=`/`-` y `Super+P`.
- Integración nativa del teléfono con KDE Connect: widget de barra, panel, tarjeta del centro de control,
  portapapeles automático e imágenes recientes (reemplaza el módulo Luau phone-connect).
- Las credenciales de VPN se piden en el diálogo propio de OKARU, sin terminal.
- Panel de sonido con volumen por aplicación y sonidos del sistema; los dispositivos se actualizan en vivo
  en Ajustes.
- Paleta propia de OKARU (azules apagados sobre grises y negro) e identidad: wallpaper 郁, logo en kanji.
- Instalador por `curl` y repositorio pacman firmado `[okaru]`.
- El agente polkit de OKARU viene activado por defecto.

#### Correcciones
- Layout scrolling: el foco y el intercambio con `Super+flechas` funcionan con una ventana maximizada o tras
  pasar una flotante a mosaico.
- En una instalación limpia los workspaces empiezan en 1 y los módulos integrados quedan activados.
- Dependencias que faltaban (fuentes Noto, accountsservice con el greeter, dependencias de compilación).
- El instalador detecta el gestor de sesión por su alias real.

#### Cambios
- Se quitaron las últimas referencias a noctalia.dev.
- El paquete declara las dependencias opcionales de los módulos integrados.
- Checks de CI obligatorios en `main`; la versión del paquete sale solo de las etiquetas `v*`.

## [1.0.0] - 2026-11-03

### English

#### Features
- First release of OKARU as an independent project.

### Español

#### Novedades
- Primera versión de OKARU como proyecto independiente.
