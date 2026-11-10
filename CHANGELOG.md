# Changelog

Each version has an English and a Spanish section, grouped by type of change:
**Features**, **Fixes**, **Regressions** (something that worked before and broke), **Changes** (behaviour,
packaging or internals). Empty groups are left out. The publish workflow copies the section of the tagged
version into its GitHub release, and Settings → Home links to it.

Cada versión tiene una sección en inglés y otra en español, separadas por tipo de cambio:
**Novedades**, **Correcciones**, **Regresiones** (algo que funcionaba y se rompió), **Cambios**
(comportamiento, empaquetado o funcionamiento interno). Los grupos vacíos se omiten. El flujo de publicación
copia la sección de la versión etiquetada en su release de GitHub, y Ajustes → Inicio enlaza a él.

## [1.2.0] - 2026-11-10

### English

#### Features
- The installer now leaves OKARU exactly as it is meant to look, on Hyprland and on Niri (without plugins or
  anything personal):
  - Bar: workspaces and media on the left; clock and weather in the center; tray, cat, phone, microphone,
    capture and control center on the right. This is also the default bar in the shell.
  - Light theme taken from the wallpaper (Material tonal spot) with the GTK, Qt, kitty, btop, cava and
    community templates (Steam, Heroic, Discord, Zen, LibreOffice, Neovim, Zed, starship, fastfetch…).
  - Control center cards, floating panels, notifications, OSD, dock and idle settings.
  - kitty configuration (padding, blur, cursor trail, powerline tabs, JetBrainsMono Bold).
  - Hyprland: niri-like scrolling layout, spring animations and slightly translucent windows.
  - Window rules for Steam (popups, main window, friends list, dialogs, toasts, games in fullscreen),
    Heroic, Faugus, Prism Launcher, the native Epic Games Launcher, Nautilus, LocalSend, file pickers and
    system utilities.
- The installer asks for the language (and, in Spanish, the country) and sets it as the system language.
- The installer adds the Chaotic-AUR repository and installs Firefox, LocalSend and what the Capture widget
  needs (gpu-screen-recorder, slurp).
- If you already have a configuration, the installer keeps it or replaces all of it (compositor, OKARU,
  Settings changes, kitty, fastfetch, starship) with a dated backup.
- Apps: open a file by dropping it from the file manager onto the calculator, viewer, player, editor or
  document viewer.
- Media player: MPRIS support, so the bar, `playerctl` and the media keys control it.

#### Changes
- Settings: the search field sits above the side menu, aligned with it; «Reset Page» moved to the header.
- Settings on Niri no longer shows the pages that only edit Hyprland (windows, input, environment,
  Hyprland plugins and diagnostics, Hyprland startup commands).
- New descriptions for the Settings modules.
- Without the phone component, the installer leaves the phone widget out of the bar.

### Español

#### Novedades
- El instalador deja OKARU tal como debe verse, en Hyprland y en Niri (sin plugins ni nada personal):
  - Barra: workspaces y multimedia a la izquierda; reloj y clima al centro; bandeja, gato, teléfono,
    micrófono, captura y centro de control a la derecha. También es la barra de fábrica de la shell.
  - Tema claro sacado del fondo (Material tonal spot) con las plantillas de GTK, Qt, kitty, btop, cava y de
    la comunidad (Steam, Heroic, Discord, Zen, LibreOffice, Neovim, Zed, starship, fastfetch…).
  - Tarjetas del centro de control, paneles flotantes, notificaciones, OSD, dock e inactividad.
  - Configuración de kitty (márgenes, desenfoque, estela del cursor, pestañas powerline, JetBrainsMono Bold).
  - Hyprland: layout scrolling tipo niri, animaciones con resortes y ventanas un poco translúcidas.
  - Reglas de ventana para Steam (popups, ventana principal, amigos, diálogos, avisos, juegos a pantalla
    completa), Heroic, Faugus, Prism Launcher, Epic Games Launcher nativo, Nautilus, LocalSend, selectores
    de archivos y utilidades del sistema.
- El instalador pregunta el idioma (y, en español, el país) y lo deja como idioma del sistema.
- El instalador agrega el repositorio Chaotic-AUR e instala Firefox, LocalSend y lo que necesita el widget
  Captura (gpu-screen-recorder, slurp).
- Si ya tienes configuración, el instalador la conserva o la reemplaza toda (compositor, OKARU, cambios de
  Ajustes, kitty, fastfetch, starship) con una copia fechada.
- Apps: abre un archivo soltándolo desde el explorador sobre la calculadora, el visor, el reproductor, el
  editor o el visor de documentos.
- Reproductor: soporte MPRIS, así la barra, `playerctl` y las teclas multimedia lo controlan.

#### Cambios
- Ajustes: la búsqueda va encima del menú lateral, alineada con él; «Restablecer página» pasó al encabezado.
- En Niri, Ajustes ya no muestra las páginas que solo editan Hyprland (ventanas, entrada, entorno, plugins y
  diagnóstico de Hyprland, comandos de inicio de Hyprland).
- Descripciones nuevas de los módulos de Ajustes.
- Sin el componente de teléfono, el instalador deja fuera de la barra el widget del teléfono.

## [1.1.1] - 2026-11-09

### English

#### Features
- Two new OKARU apps, with the same look as the others (your color theme, font and scale, live):
  - **Text Editor** (`okaru-edit`), modeled on GNOME Text Editor: tabs, find and replace, go to line, undo
    and redo, smart indentation, comment lines (Ctrl+/), move and duplicate lines, word wrap, line numbers,
    unsaved-changes warning, recent files, and the open files come back next time. Syntax highlighting for
    HTML, XML, JSON, JSONC, TOML, CSS, conf/ini, Lua, KDL and CSV, with colors taken from your palette. JSON,
    JSONC, TOML and XML are checked as you type and the line with the error is marked; JSON, JSONC and XML
    can be formatted (Ctrl+Shift+F), and CSV can be shown as a table. Saving is atomic and keeps the file's
    permissions and symlinks; Latin-1 files are converted to UTF-8; files changed outside the editor reload.
  - **Document Viewer** (`okaru-pdf`), modeled on Papers and built on poppler: tabs, a sidebar with page
    thumbnails or the document outline, continuous scrolling in one or two columns, zoom (fit width, fit page,
    steps, Ctrl+wheel under the pointer), rotate, night mode, search the whole document with every result
    highlighted, go to a page by number or by its printed label, select and copy text, links, password-
    protected PDFs, properties, printing through CUPS (printer, copies, page range, two-sided) and a
    presentation mode (F5). It remembers the page and zoom of every file. Even very large documents open
    and scroll smoothly.
- Photos open with the Image Viewer, also on a fresh install: it now handles 69 image types (adding EXR,
  JPEG 2000, PSD, DDS, PCX, SGI, Sun raster, WBMP, XBM, APNG and every camera RAW format), and OKARU ships
  default apps for Hyprland and Niri: images to the viewer, text files to the editor and PDF to the document
  viewer. Whatever you choose in `~/.config/mimeapps.list` still wins.
- Niri and Hyprland default configuration: the editor and the document viewer open floating and centered.

#### Fixes
- Text drawn with markup colors (for example highlighted code) kept its colors instead of being tinted with a
  single one.

#### Changes
- New package dependency: `poppler-glib`. Optional: `cups` to print from the document viewer.
- Tabs are a shared component of the apps (editor and document viewer).
- If you already have OKARU's configuration, add the rules for `dev.okaru.Editor` and `dev.okaru.Pdf` from
  `/usr/share/okaru/defaults`.

### Español

#### Novedades
- Dos apps nuevas de OKARU, con el mismo aspecto que las demás (tu tema de color, fuente y escala, en vivo):
  - **Editor de texto** (`okaru-edit`), inspirado en GNOME Text Editor: pestañas, buscar y reemplazar, ir a
    una línea, deshacer y rehacer, sangría automática, comentar líneas (Ctrl+/), mover y duplicar líneas,
    ajuste de línea, números de línea, aviso de cambios sin guardar, archivos recientes, y los archivos
    abiertos vuelven la próxima vez. Resaltado de HTML, XML, JSON, JSONC, TOML, CSS, conf/ini, Lua, KDL y
    CSV, con colores sacados de tu paleta. JSON, JSONC, TOML y XML se validan mientras escribes y se marca la
    línea del error; JSON, JSONC y XML se pueden formatear (Ctrl+Shift+F), y un CSV se puede ver como tabla.
    Guarda de forma atómica respetando los permisos y los enlaces del archivo; los archivos en Latin-1 pasan
    a UTF-8; si el archivo cambia desde fuera, se recarga.
  - **Visor de documentos** (`okaru-pdf`), inspirado en Papers y hecho sobre poppler: pestañas, barra
    lateral con miniaturas de las páginas o el índice del documento, desplazamiento continuo en una o dos
    columnas, zoom (ajustar al ancho o a la página, pasos, Ctrl+rueda bajo el puntero), girar, modo nocturno,
    buscar en todo el documento con cada resultado resaltado, ir a una página por número o por su número
    impreso, seleccionar y copiar texto, enlaces, PDF con contraseña, propiedades, imprimir con CUPS
    (impresora, copias, rango de páginas, doble cara) y modo presentación (F5). Recuerda la página y el zoom
    de cada archivo. Hasta los documentos muy grandes abren y se desplazan con fluidez.
- Las fotos se abren con el visor de imágenes, también en una instalación nueva: ahora maneja 69 tipos de
  imagen (se suman EXR, JPEG 2000, PSD, DDS, PCX, SGI, Sun raster, WBMP, XBM, APNG y todos los RAW de cámara),
  y OKARU trae apps por defecto para Hyprland y Niri: imágenes al visor, texto al editor y PDF al visor de
  documentos. Lo que elijas en `~/.config/mimeapps.list` sigue mandando.
- Configuración de fábrica de Hyprland y Niri: el editor y el visor de documentos abren flotantes y
  centrados.

#### Correcciones
- El texto dibujado con colores en el markup (por ejemplo, código resaltado) conserva sus colores en vez de
  teñirse de uno solo.

#### Cambios
- Dependencia nueva del paquete: `poppler-glib`. Opcional: `cups` para imprimir desde el visor de documentos.
- Las pestañas son un componente compartido de las apps (editor y visor de documentos).
- Si ya tienes la configuración de OKARU, agrega las reglas de `dev.okaru.Editor` y `dev.okaru.Pdf` desde
  `/usr/share/okaru/defaults`.

## [1.1.0] - 2026-11-09

### English

#### Features
- OKARU now has its own apps. Each one is a separate program with OKARU's look: it follows your color theme
  (light/dark and palette, live), your font and your scale, and uses your Validate/Cancel keys.
  - **Calculator** (`okaru-calc`), modeled on GNOME Calculator: basic, advanced, financial (GNOME's ten
    functions), programming (binary/octal/decimal/hex, word size, AND/OR/XOR/NOT, shifts, bit panel) and
    conversion modes (every libqalculate unit and currency, including less common ones such as the Colombian
    peso, with exchange rates updated on demand). Live result preview, history, `ans`, variables
    (`name = value`) and preferences (angle unit, number format, decimal places, trailing zeroes, thousands
    separators).
  - **Image Viewer** (`okaru-view`): browses the folder of the image you open, zooms under the pointer with
    the wheel, keys or double click, drag to pan, rotate, fullscreen, properties with camera data, copy, set as
    wallpaper, move to trash and show in folder. Opens PNG, JPEG, GIF and WebP (animated too), JXL, HEIF/AVIF,
    BMP, ICO, QOI, TGA, PNM, TIFF, XPM, ICNS, sharp SVG at any zoom and camera RAW. Large photos load at screen
    size first and the next and previous images are decoded ahead, so browsing is instant.
  - **Media Player** (`okaru-play`), on mpv: every format ffmpeg plays, GPU decoding, audio and subtitle
    tracks, speed, volume, the folder as a playlist that moves on by itself, fullscreen, cover and tags for
    music, and it resumes where you left off. The controls hide when the pointer is still or outside the
    window.
- Niri: display settings follow the monitor (make, model and serial) instead of the port it is plugged into,
  and a disconnected monitor keeps its settings.
- Terminal: an OKARU starship prompt of its own (user, path, git status with counters, command duration,
  per-language icons, 郁 as the symbol).

#### Fixes
- fastfetch showed some icons as blanks (PC, CPU, GPU, RAM, system, kernel, WM, terminal, font and Update);
  the kanji is centered again.
- Text fields that ask for right alignment now get it (the value next to the sliders in the desktop widgets
  editor).

#### Changes
- New package dependencies: `mpv`, `exiv2`, `gdk-pixbuf2`, `libraw`, `libjpeg-turbo` and `wl-clipboard`.
- The shell writes the active palette to `~/.local/state/okaru/palette.json`, which the apps follow.
- The default Hyprland and Niri configuration opens the apps floating and centered. If you already have
  OKARU's configuration, add the rules for `dev.okaru.Calculator`, `dev.okaru.Viewer` and `dev.okaru.Player`
  from `/usr/share/okaru/defaults`.

### Español

#### Novedades
- OKARU ahora tiene apps propias. Cada una es un programa aparte con el aspecto de OKARU: sigue tu tema de
  color (claro/oscuro y paleta, en vivo), tu fuente y tu escala, y usa tus teclas de validar y cancelar.
  - **Calculadora** (`okaru-calc`), inspirada en la de GNOME: modos básico, avanzado, financiero (las diez
    funciones de GNOME), programación (binario/octal/decimal/hexadecimal, tamaño de palabra,
    AND/OR/XOR/NOT, desplazamientos, panel de bits) y conversión (todas las unidades y divisas de
    libqalculate, también las menos comunes como el peso colombiano, con tasas de cambio que se actualizan al
    pedirlas). Vista previa del resultado mientras escribes, historial, `ans`, variables (`nombre = valor`) y
    preferencias (unidad de ángulo, formato de números, decimales, ceros finales, separador de miles).
  - **Visor de imágenes** (`okaru-view`): recorre la carpeta de la imagen que abres, acerca bajo el puntero
    con la rueda, el teclado o doble clic, se mueve arrastrando, gira, pantalla completa, propiedades con los
    datos de la cámara, copiar, usar como fondo de pantalla, mover a la papelera y mostrar en la carpeta. Abre
    PNG, JPEG, GIF y WebP (también animados), JXL, HEIF/AVIF, BMP, ICO, QOI, TGA, PNM, TIFF, XPM, ICNS, SVG
    nítido a cualquier zoom y RAW de cámara. Las fotos grandes cargan primero al tamaño de la pantalla y la
    siguiente y la anterior se decodifican de antemano, así que pasar de una a otra es inmediato.
  - **Reproductor multimedia** (`okaru-play`), sobre mpv: todos los formatos que reproduce ffmpeg,
    decodificación por GPU, pistas de audio y subtítulos, velocidad, volumen, la carpeta como lista que avanza
    sola, pantalla completa, portada y datos de la música, y retoma donde lo dejaste. Los controles se ocultan
    con el puntero quieto o fuera de la ventana.
- Niri: los ajustes de pantalla siguen al monitor (marca, modelo y serie) en vez del puerto donde está
  conectado, y un monitor desconectado conserva sus ajustes.
- Terminal: un prompt de starship propio de OKARU (usuario, ruta, estado de git con contadores, duración de
  los comandos, iconos por lenguaje, 郁 como símbolo).

#### Correcciones
- fastfetch mostraba algunos iconos en blanco (PC, CPU, GPU, RAM, sistema, kernel, WM, terminal, fuente y
  Update); el kanji vuelve a quedar centrado.
- Los campos de texto que piden alineación a la derecha ahora la tienen (el valor junto a los deslizadores en
  el editor de widgets del escritorio).

#### Cambios
- Dependencias nuevas del paquete: `mpv`, `exiv2`, `gdk-pixbuf2`, `libraw`, `libjpeg-turbo` y `wl-clipboard`.
- La shell escribe la paleta activa en `~/.local/state/okaru/palette.json`, que siguen las apps.
- La configuración de fábrica de Hyprland y Niri abre las apps flotantes y centradas. Si ya tienes la
  configuración de OKARU, agrega las reglas de `dev.okaru.Calculator`, `dev.okaru.Viewer` y `dev.okaru.Player`
  desde `/usr/share/okaru/defaults`.

## [1.0.5] - 2026-11-09

### English

#### Features
- fastfetch and starship in OKARU's style, based on Omarchy's: fastfetch shows the 郁 kanji next to the
  Hardware, Software and Age / Uptime / Update boxes, and the starship prompt uses 郁. The installer adds both
  packages and copies their configuration only if you don't have one.
- The installer adds `starship init` at the end of your login shell's file (fish, zsh or bash), after the
  CachyOS config it sources, so it is not overridden; on zsh it unloads CachyOS's powerlevel10k.
- The installer speaks English and Spanish: it follows the system language, and `OKARU_LANG=en` or
  `OKARU_LANG=es` forces one (`curl -fsSL …/install.sh | OKARU_LANG=en bash`).

#### Fixes
- The launcher's profile picture was blurry with photos that are not square; it now loads at double size,
  like the control center.

#### Changes
- The shell no longer writes `[session.power]` and `[[session.actions]]` to the greeter's `sync.toml`; the
  greeter has not used them since 1.0.4.

### Español

#### Novedades
- fastfetch y starship con el estilo de OKARU, basados en los de Omarchy: fastfetch muestra el kanji 郁 junto a
  las cajas Hardware, Software y Age / Uptime / Update, y el prompt de starship usa 郁. El instalador agrega
  los dos paquetes y copia su configuración solo si no tienes una.
- El instalador agrega `starship init` al final del archivo de tu shell de inicio de sesión (fish, zsh o bash),
  después del source de la config de CachyOS, para que no lo pise; en zsh descarga el powerlevel10k de CachyOS.
- El instalador habla inglés y español: sigue el idioma del sistema, y `OKARU_LANG=en` u `OKARU_LANG=es` fuerza
  uno (`curl -fsSL …/install.sh | OKARU_LANG=en bash`).

#### Correcciones
- La foto de perfil del lanzador se veía borrosa con fotos que no son cuadradas; ahora se carga al doble de
  tamaño, como en el centro de control.

#### Cambios
- La shell ya no escribe `[session.power]` ni `[[session.actions]]` en el `sync.toml` del greeter, que no los
  usa desde la 1.0.4.

## [1.0.4] - 2026-11-08

### English

#### Features
- Screen recording: while recording, the Capture widget shows the elapsed time and buttons to pause/resume and
  finish. The time does not advance while paused. The capture panel has the same Pause/Resume button, and
  `okaru msg capture-pause-toggle` can be bound to a shortcut.
- Panels now appear with a quick fade, like Omarchy (about 140 ms in, 110 ms out), instead of unfolding
  downwards.
- Session menu: actions are confirmed by holding their button, their key or Enter for 300 ms, as in dux-shell.
  The button fills while held; releasing early shows "Hold to confirm".
- Font weight: Settings → Appearance → Interface has a Font Weight selector that lists only the weights the
  chosen font has. All shell text follows it and bold text keeps its difference.
- Apply font to the whole system (on by default): changing the font or its weight also applies it to GTK, Qt
  (qt5ct/qt6ct), kitty (open windows reload, so starship and other terminal apps follow), VS Code, Zed and
  ghostty, using the font's Mono variant for terminals and editors.
- JetBrainsMono Nerd Font is the default font of the shell, and the installer sets it for the whole system
  (fontconfig, GTK and kitty).
- The Hyprland screen-sharing picker (xdg-desktop-portal-hyprland 1.5) follows the OKARU palette through the
  new Hyprtoolkit template.

#### Fixes
- File pickers that apps open without the portal (for example Epic Games Launcher) now float, centered, at
  900×600.
- The System panel cut the CPU load and disk names ("CP…", "… tra"); it is now wider.
- Capture widget icons were one pixel lower than the other bar icons.
- With a heavier font, the Settings sidebar cut its descriptions; it now widens to fit.
- Greeter: Tab and Enter could reach the hidden power buttons of the old panel (shut down, reboot).

#### Changes
- Settings: the "Override" badge is gone; settings changed from their default show only the revert icon,
  the same control as the OKARU pages, with the confirmation in the same row.
- Lock screen: removed the old login box, the lock screen widgets editor (and its
  `lockscreen-widgets-edit/exit/toggle-edit` commands) and `lockscreen.tint_intensity`; config migration 22
  drops them from `settings.toml`.
- Session menu: `countdown_seconds` was removed (config migration 23).
- Greeter: removed the old panel, its menus, clock and power buttons, and the options `[clock]`,
  `[appearance].hide_logo`, `password_style`, `power_buttons_position` and `scheme_selector_position`.
  Keyboard: Enter logs in, F3 / Shift+F3 change the session.
- New dependency: `ttf-jetbrains-mono-nerd`.
- Internal cleanup: about 8,000 lines of unused code removed between the shell and the greeter.

### Español

#### Novedades
- Grabación de pantalla: mientras graba, el widget Captura muestra el tiempo y botones para pausar/reanudar y
  terminar. El tiempo no avanza en pausa. El panel de captura tiene el mismo botón Pausar/Reanudar, y
  `okaru msg capture-pause-toggle` se puede asignar a un atajo.
- Los paneles aparecen con un fundido rápido, como en Omarchy (unos 140 ms al abrir y 110 ms al cerrar), en
  lugar de desplegarse hacia abajo.
- Menú de sesión: las acciones se confirman manteniendo presionado su botón, su tecla o Enter durante 300 ms,
  como en dux-shell. El botón se llena mientras tanto; si se suelta antes, avisa «Mantén presionado para
  confirmar».
- Grosor de la letra: Ajustes → Apariencia → Interfaz tiene un selector de grosor con solo los pesos que tiene
  la fuente elegida. Todo el texto de la shell lo sigue y la negrita conserva su diferencia.
- Aplicar la fuente a todo el sistema (activado por defecto): al cambiar la fuente o su grosor se aplica
  también en GTK, Qt (qt5ct/qt6ct), kitty (las ventanas abiertas se recargan, así que starship y demás apps de
  terminal la siguen), VS Code, Zed y ghostty, con la variante Mono para terminales y editores.
- JetBrainsMono Nerd Font es la fuente por defecto de la shell, y el instalador la configura en todo el sistema
  (fontconfig, GTK y kitty).
- El selector para compartir pantalla de Hyprland (xdg-desktop-portal-hyprland 1.5) sigue la paleta de OKARU con
  la nueva plantilla Hyprtoolkit.

#### Correcciones
- Los selectores de archivos que las apps abren sin el portal (por ejemplo Epic Games Launcher) ahora flotan,
  centrados, a 900×600.
- El panel Sistema cortaba la carga de CPU y los nombres de los discos («CP…», «… tra»); ahora es más ancho.
- Los iconos del widget Captura quedaban un píxel más abajo que los demás iconos de la barra.
- Con una letra más gruesa, la barra lateral de Ajustes cortaba sus frases; ahora se ensancha para que quepan.
- Greeter: Tab y Enter podían llegar a los botones de energía ocultos del panel antiguo (apagar, reiniciar).

#### Cambios
- Ajustes: ya no aparece la etiqueta «Override»; los ajustes cambiados respecto al valor por defecto solo
  muestran el icono de revertir, el mismo control de las páginas de OKARU, con la confirmación en la misma fila.
- Pantalla de bloqueo: se quitaron la caja de login antigua, el editor de widgets del bloqueo (y sus comandos
  `lockscreen-widgets-edit/exit/toggle-edit`) y `lockscreen.tint_intensity`; la migración 22 los borra de
  `settings.toml`.
- Menú de sesión: se quitó `countdown_seconds` (migración 23).
- Greeter: se quitaron el panel antiguo, sus menús, el reloj y los botones de energía, y las opciones `[clock]`,
  `[appearance].hide_logo`, `password_style`, `power_buttons_position` y `scheme_selector_position`. Teclado:
  Enter inicia sesión, F3 / Shift+F3 cambian de sesión.
- Nueva dependencia: `ttf-jetbrains-mono-nerd`.
- Limpieza interna: unas 8.000 líneas de código sin uso eliminadas entre la shell y el greeter.

## [1.0.3] - 2026-11-07

### English

#### Features
- New lock screen in the style of Omarchy: the wallpaper heavily blurred under a dark veil, the 郁 + OKARU
  logo from the installer (kanji in the accent color), the user and password in a pixel font, and logout,
  suspend, reboot and shutdown as pixel icons.
- The greeter has the same design as the lock screen, with a session selector at the bottom (click or scroll).
- Settings → Home shows the 郁 + OKARU logo at the top.
- Control center: change the profile picture from the profile card (hover the photo; up to 6 MB).
- Niri: Settings → Displays and Workspaces now work on Niri. OKARU writes its own `niri/okaru/outputs.kdl` and
  `workspaces.kdl`, checked with `niri validate`, and offers to add the two `include` lines to `config.kdl`.
- Settings → Home shows Niri and its version as the compositor.

#### Fixes
- The profile picture in the control center looked blurry.
- The weather panel cut the day names in the forecast and wrapped the time zone onto two lines.
- On Niri, Settings → Home showed the compositor as "Not detected".

#### Changes
- Keyring secrets are now stored as OKARU (`dev.okaru.Secret`); the ones saved under the old name are migrated
  automatically the first time they are read.
- New dependency: `ttf-bigblueterminal-nerd` (okaru and okaru-greeter).
- Lock screen: stronger blur by default; the tint and lock screen widgets settings were removed because the new
  design no longer shows them.
- Settings: removed per-monitor bar options and bar capsule styles that could no longer take effect.
- The stock Niri config includes the OKARU display and workspace files.

### Español

#### Novedades
- Pantalla de bloqueo nueva al estilo de Omarchy: el fondo de pantalla muy desenfocado bajo un velo oscuro,
  el logo 郁 + OKARU del instalador (el kanji en el color de acento), el usuario y la contraseña en tipografía
  pixelada, y cerrar sesión, suspender, reiniciar y apagar como iconos pixelados.
- El greeter tiene el mismo diseño que la pantalla de bloqueo, con un selector de sesión abajo (clic o rueda).
- Ajustes → Inicio muestra el logo 郁 + OKARU arriba.
- Centro de control: cambiar la foto de perfil desde la tarjeta de perfil (pasar el puntero por la foto;
  hasta 6 MB).
- Niri: Ajustes → Pantallas y Workspaces ya funcionan en Niri. OKARU escribe sus propios
  `niri/okaru/outputs.kdl` y `workspaces.kdl`, comprobados con `niri validate`, y ofrece añadir las dos líneas
  `include` a `config.kdl`.
- Ajustes → Inicio muestra Niri y su versión como compositor.

#### Correcciones
- La foto de perfil del centro de control se veía borrosa.
- El panel del clima cortaba los nombres de los días del pronóstico y partía la zona horaria en dos líneas.
- En Niri, Ajustes → Inicio mostraba el compositor como "No detectado".

#### Cambios
- Los secretos del llavero se guardan ahora como OKARU (`dev.okaru.Secret`); los guardados con el nombre
  anterior se migran solos la primera vez que se leen.
- Nueva dependencia: `ttf-bigblueterminal-nerd` (okaru y okaru-greeter).
- Pantalla de bloqueo: más desenfoque por defecto; se quitaron de Ajustes el tinte y los widgets de la pantalla
  de bloqueo, que el diseño nuevo ya no muestra.
- Ajustes: se quitaron opciones de la barra por monitor y estilos de cápsulas que ya no podían tener efecto.
- La configuración de fábrica de Niri incluye los archivos de pantallas y workspaces de OKARU.

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
