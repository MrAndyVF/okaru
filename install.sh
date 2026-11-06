#!/usr/bin/env bash
# 郁 OKARU Linux Shell — instalador para Arch Linux y derivadas, sin compilar nada.
#
#   curl -fsSL https://raw.githubusercontent.com/MrAndyVF/okaru/main/install.sh | bash
#
# Asistente en la terminal (con gum): elige componentes, revisa los paquetes, instala desde el repositorio
# pacman [okaru] (paquetes firmados) y deja la configuración de fábrica del compositor elegido, Hyprland
# o Niri (nada personal).
set -euo pipefail

readonly REPO_URL="https://github.com/MrAndyVF/okaru/releases/download/repo"
readonly KEY_URL="https://raw.githubusercontent.com/MrAndyVF/okaru/main/packaging/keys/okaru.asc"
# Huella de la llave que firma los paquetes; debe coincidir con packaging/keys/okaru.asc.
readonly KEY_FPR="1043639C3CDC77859031AAADBDE4201B61FDE527"
readonly DEFAULTS_ROOT="/usr/share/okaru/defaults"
readonly COMMON_PACKAGES=(kitty nautilus xdg-desktop-portal-gtk noto-fonts)
# Lo básico de cada compositor: sesión, portal de capturas/compartir pantalla y, en Niri, apps X11.
readonly HYPR_PACKAGES=(hyprland uwsm xdg-desktop-portal-hyprland)
readonly NIRI_PACKAGES=(niri xdg-desktop-portal-gnome xwayland-satellite)
readonly PHONE_PACKAGES=(kdeconnect sshfs)
# gum (la interfaz) se descarga a una carpeta temporal y se verifica; no queda instalado.
readonly GUM_VERSION="2.0.2"
readonly GUM_SHA256="d842e06d93dbed90af48cb8dd10698db6f22e331fc40346bb37bbc753109edc2"

# Paleta OKARU
readonly C_PRIMARY="#7095C8" C_MUTED="#9AA4B2" C_OK="#7FA8C4" C_ERR="#D9707A" C_TEXT="#E6E9EF"

readonly OPT_HYPR="Hyprland (con uwsm)"
readonly OPT_NIRI="Niri"
readonly OPT_GREETER="OKARU Greeter (pantalla de inicio de sesión)"
readonly OPT_PHONE="Teléfono con KDE Connect"
readonly OPT_EXTRA="Dependencias opcionales (grabación, VPN, actualizaciones…)"

LOG="${XDG_CACHE_HOME:-$HOME/.cache}/okaru/install-$(date +%Y%m%d-%H%M%S).log"
TMP="" GUM="" KEEPALIVE=""
WANT_GREETER=false WANT_PHONE=false WANT_OPTIONAL=false SWITCH_DM="" CONFIG_ACTION=keep
COMPOSITOR=hyprland
EXTRA=()

plain_die() { printf '\e[31;1merror:\e[0m %s\n' "$*" >&2; exit 1; }

cleanup() {
  [[ -n $KEEPALIVE ]] && kill "$KEEPALIVE" 2>/dev/null
  [[ -n $TMP ]] && rm -rf "$TMP"
  return 0
}
trap cleanup EXIT

checks() {
  [[ $EUID -ne 0 ]] || plain_die "ejecútalo como tu usuario (pedirá la contraseña cuando haga falta), no como root."
  command -v pacman >/dev/null || plain_die "este instalador es para Arch Linux y derivadas (no se encontró pacman)."
  [[ $(uname -m) == x86_64 ]] || plain_die "los paquetes de OKARU solo existen para x86_64."
  { : </dev/tty; } 2>/dev/null || plain_die "se necesita una terminal interactiva."
  command -v sudo >/dev/null || plain_die "se necesita sudo."
}

bootstrap_gum() {
  printf '\e[2mPreparando el instalador…\e[0m\n'
  TMP=$(mktemp -d)
  local tarball="$TMP/gum.tar.gz"
  curl -fsSL "https://github.com/charmbracelet/gum/releases/download/v${GUM_VERSION}/gum_${GUM_VERSION}_Linux_x86_64.tar.gz" \
    -o "$tarball" || plain_die "no se pudo descargar gum (¿hay conexión a internet?)."
  echo "$GUM_SHA256  $tarball" | sha256sum -c --status || plain_die "gum descargado no coincide con su suma de verificación."
  tar -xzf "$tarball" -C "$TMP"
  GUM=$(find "$TMP" -type f -name gum -perm -u+x | head -n1)
  [[ -n $GUM ]] || plain_die "no se encontró gum dentro del archivo descargado."
  mkdir -p "$(dirname "$LOG")"
  : >"$LOG"
}

# --- Piezas de interfaz ---------------------------------------------------------------------------------

title() { "$GUM" style --bold --foreground "$C_PRIMARY" --margin "1 0 0 0" "$1"; }
note() { "$GUM" style --foreground "$C_MUTED" "$@"; }
confirm() {
  "$GUM" confirm --prompt.foreground "$C_PRIMARY" --selected.background "$C_PRIMARY" --selected.foreground "#0B0D11" "$@"
}
ok() { printf '  %s %s\n' "$("$GUM" style --foreground "$C_OK" ✓)" "$1"; }

banner() {
  clear
  local kanji name
  # El kanji 郁 sale del SVG del logo (okaru-greeter/assets/okaru.svg) rasterizado a medios bloques.
  kanji=$("$GUM" style --foreground "$C_PRIMARY" --bold --padding "0 3 0 1" \
    '      ██       ▄▄▄▄▄▄▄' \
    '  ▄██████████▄██▀▀▀███' \
    '     ██       ██   ██' \
    '   ▄█████████ ██  ██' \
    '  ▄███     ██ ██  ██' \
    '  ▀▀█████████ ██   ▀█▄' \
    '    ██▄▄▄▄▄██ ██    ██' \
    '    ██▀▀▀▀▀██ ██  ▄▄██' \
    '    ██     ██ ██ ▀▀▀▀' \
    '    ██  ▀███▀ ██')
  name=$("$GUM" join --vertical \
    "$("$GUM" style --foreground "$C_TEXT" --bold \
      ' ██████╗ ██╗  ██╗ █████╗ ██████╗ ██╗   ██╗' \
      '██╔═══██╗██║ ██╔╝██╔══██╗██╔══██╗██║   ██║' \
      '██║   ██║█████╔╝ ███████║██████╔╝██║   ██║' \
      '██║   ██║██╔═██╗ ██╔══██║██╔══██╗██║   ██║' \
      '╚██████╔╝██║  ██╗██║  ██║██║  ██║╚██████╔╝' \
      ' ╚═════╝ ╚═╝  ╚═╝╚═╝  ╚═╝╚═╝  ╚═╝ ╚═════╝ ')" \
    "$("$GUM" style --foreground "$C_MUTED" --italic --margin "1 0 0 1" "OKARU Linux Shell — instalador")")
  "$GUM" join --horizontal --align center "$kanji" "$name"
}

# Pantalla de error con las últimas líneas del registro; termina el instalador.
fail() {
  title "$("$GUM" style --foreground "$C_ERR" "La instalación falló")"
  "$GUM" style --foreground "$C_ERR" "$1"
  if [[ -s $LOG ]]; then
    printf '\n'
    note "Últimas líneas del registro:"
    tail -n 15 "$LOG" | sed 's/^/  /'
    note "" "Registro completo: $LOG"
  fi
  exit 1
}

# run_step "Título" comando… — corre el comando guardando su salida en el registro y muestra en vivo
# las últimas líneas bajo un indicador de progreso, como un asistente gráfico.
run_step() {
  local label=$1 start pid rc=0 i=0 drawn=0 cols line
  shift
  local frames=(⠋ ⠙ ⠹ ⠸ ⠼ ⠴ ⠦ ⠧ ⠇ ⠏)
  printf '\n== %s\n' "$label" >>"$LOG"
  start=$(wc -l <"$LOG")
  "$@" >>"$LOG" 2>&1 </dev/null &
  pid=$!
  cols=$(($(tput cols 2>/dev/null || echo 80) - 6))
  printf '\e[?25l'
  while kill -0 "$pid" 2>/dev/null; do
    ((drawn)) && printf '\e[%dA' "$drawn"
    printf '\e[J  \e[38;2;112;149;200m%s\e[0m %s\n' "${frames[i++ % ${#frames[@]}]}" "$label"
    drawn=1
    while IFS= read -r line; do
      printf '    \e[2m%s\e[0m\n' "${line:0:cols}"
      drawn=$((drawn + 1))
    done < <(tail -n +$((start + 1)) "$LOG" | tr '\r' '\n' | grep -v '^[[:space:]]*$' | tail -n 6)
    sleep 0.12
  done
  wait "$pid" || rc=$?
  ((drawn)) && printf '\e[%dA\e[J' "$drawn"
  printf '\e[?25h'
  if ((rc)); then
    printf '  %s %s\n' "$("$GUM" style --foreground "$C_ERR" ✗)" "$label"
    fail "Falló: $label (código $rc)"
  fi
  ok "$label"
}

# --- Pantallas ------------------------------------------------------------------------------------------

welcome() {
  banner
  local distro
  distro=$(. /etc/os-release 2>/dev/null && echo "${PRETTY_NAME:-Linux}")
  printf '\n'
  "$GUM" style --border rounded --border-foreground "$C_PRIMARY" --padding "0 2" "Sistema: $distro / $(uname -m)"
  title "QUÉ OBTIENES"
  "$GUM" style --foreground "$C_TEXT" \
    "  • OKARU Shell: barra, panel de control, lanzador, notificaciones y ajustes" \
    "  • Hyprland (con uwsm) o Niri, kitty como terminal y Nautilus como gestor de archivos" \
    "  • Reglas de ventana y atajos estándar de OKARU, sin configuración personal" \
    "  • Paquetes firmados desde el repositorio [okaru]: se actualizan con pacman -Syu"
  printf '\n'
  confirm --affirmative "Continuar" --negative "Salir" "¿Empezamos?" || exit 0
}

choose_compositor() {
  title "Compositor"
  note "OKARU funciona sobre los dos; cada uno se instala con lo básico para funcionar."
  local picked
  picked=$("$GUM" choose --cursor.foreground "$C_PRIMARY" --selected.foreground "$C_PRIMARY" \
    "$OPT_HYPR" "$OPT_NIRI") || exit 0
  [[ $picked == "$OPT_NIRI" ]] && COMPOSITOR=niri
  return 0
}

# Carpeta de configuración del compositor elegido y su nombre para mostrar.
config_target() { echo "${XDG_CONFIG_HOME:-$HOME/.config}/$([[ $COMPOSITOR == niri ]] && echo niri || echo hypr)"; }
compositor_name() { [[ $COMPOSITOR == niri ]] && echo Niri || echo Hyprland; }
defaults_dir() { echo "$DEFAULTS_ROOT/$([[ $COMPOSITOR == niri ]] && echo niri || echo hypr)"; }

choose_components() {
  title "Componentes"
  note "Espacio marca o desmarca, Enter confirma."
  local picked
  picked=$("$GUM" choose --no-limit --cursor.foreground "$C_PRIMARY" --selected.foreground "$C_PRIMARY" \
    --selected "$OPT_GREETER" --selected "$OPT_PHONE" \
    "$OPT_GREETER" "$OPT_PHONE" "$OPT_EXTRA") || exit 0
  [[ $picked == *"$OPT_GREETER"* ]] && WANT_GREETER=true
  [[ $picked == *"$OPT_PHONE"* ]] && WANT_PHONE=true
  [[ $picked == *"$OPT_EXTRA"* ]] && WANT_OPTIONAL=true
  return 0
}

authenticate() {
  title "Permisos de administrador"
  if ! sudo -n true 2>/dev/null; then
    local tries pass
    for tries in 1 2 3; do
      pass=$("$GUM" input --password --placeholder "contraseña de $USER" --prompt "🔒 ") || exit 0
      printf '%s\n' "$pass" | sudo -S -p '' -v 2>/dev/null && break
      unset pass
      ((tries == 3)) && fail "No se pudo verificar la contraseña de sudo."
      "$GUM" style --foreground "$C_ERR" "Contraseña incorrecta, intenta de nuevo."
    done
    unset pass
  fi
  # Mantiene vivo el permiso de sudo mientras dura la instalación.
  (while sleep 50; do sudo -n -v 2>/dev/null || exit; done) &
  KEEPALIVE=$!
  ok "Permiso concedido"
}

add_repo() {
  local key="$TMP/okaru.asc"
  curl -fsSL "$KEY_URL" -o "$key"
  # La llave descargada debe ser exactamente la que está fijada en este script.
  if ! gpg --show-keys --with-colons "$key" 2>/dev/null | grep -q "^fpr:::::::::${KEY_FPR}:"; then
    echo "la llave descargada no coincide con la huella esperada (${KEY_FPR})."
    return 1
  fi
  sudo pacman-key --add "$key"
  sudo pacman-key --lsign-key "$KEY_FPR"
  if grep -q '^\[okaru\]' /etc/pacman.conf; then
    echo "[okaru] ya estaba en /etc/pacman.conf"
  else
    printf '\n[okaru]\nServer = %s\n' "$REPO_URL" | sudo tee -a /etc/pacman.conf
  fi
  sudo pacman -Sy
}

optional_packages() {
  # Opcionales de okaru que existen en los repos oficiales (las de AUR o de paquetes propios no pasan el filtro).
  LC_ALL=C pacman -Si okaru 2>/dev/null | awk '
    /^Optional Deps/ { on = 1; sub(/^Optional Deps *: */, "") }
    on && /^[A-Z][A-Za-z ]* *: / && !/^Optional Deps/ { exit }
    on { sub(/^ +/, ""); split($0, parts, ":"); if (parts[1] != "None") print parts[1] }
  ' | while read -r name; do
    pacman -Si "$name" >/dev/null 2>&1 && printf '%s\n' "$name"
  done
}

package_list() {
  local packages=("${COMMON_PACKAGES[@]}" okaru)
  if [[ $COMPOSITOR == niri ]]; then packages+=("${NIRI_PACKAGES[@]}"); else packages+=("${HYPR_PACKAGES[@]}"); fi
  $WANT_PHONE && packages+=("${PHONE_PACKAGES[@]}")
  $WANT_GREETER && packages+=(greetd okaru-greeter accountsservice)
  printf '%s\n' "${packages[@]}"
}

review() {
  title "Revisión de paquetes"
  local pkg
  while read -r pkg; do
    if pacman -Q "$pkg" >/dev/null 2>&1; then
      printf '  %s %-30s %s\n' "$("$GUM" style --foreground "$C_OK" ✓)" "$pkg" "$(note "ya instalado")"
    else
      printf '  %s %-30s %s\n' "$("$GUM" style --foreground "$C_PRIMARY" +)" "$pkg" "$(note "se instalará")"
    fi
  done < <(package_list)
  if $WANT_OPTIONAL; then
    mapfile -t EXTRA < <(optional_packages)
    ((${#EXTRA[@]})) && note "" "  Opcionales: ${EXTRA[*]}"
  fi
  note "" "  También se actualiza el sistema (pacman -Syu), como pide Arch al instalar paquetes."

  if $WANT_GREETER; then
    local current=""
    # Sin gestor de sesión, systemctl show devuelve igual «display-manager.service»: se mira el alias real.
    local link=/etc/systemd/system/display-manager.service
    [[ -L $link ]] && current=$(basename "$(readlink -f "$link")")
    if [[ -n $current && $current != greetd.service ]]; then
      printf '\n'
      if confirm --affirmative "Cambiar" --negative "Mantener" \
        "Ahora inicias sesión con ${current%.service}. ¿Cambiar a OKARU Greeter?"; then
        SWITCH_DM=$current
      else
        SWITCH_DM=keep
      fi
    fi
  fi

  local target
  target=$(config_target)
  CONFIG_ACTION=copy
  if [[ -e $target ]]; then
    title "Configuración de $(compositor_name)"
    note "Ya tienes ${target/#$HOME/\~}. Reemplazarla deja las reglas y atajos de OKARU; se guarda una copia con fecha."
    local choice
    choice=$("$GUM" choose --cursor.foreground "$C_PRIMARY" "Conservar la mía" "Reemplazar por la de OKARU") || exit 0
    [[ $choice == Reemplazar* ]] && CONFIG_ACTION=replace || CONFIG_ACTION=keep
  fi

  printf '\n'
  confirm --affirmative "Instalar" --negative "Cancelar" "¿Instalar OKARU?" || exit 0
}

install_packages() {
  local packages
  mapfile -t packages < <(package_list)
  sudo pacman -Syu --needed --noconfirm "${packages[@]}"
  if ((${#EXTRA[@]})); then
    sudo pacman -S --needed --noconfirm --asdeps "${EXTRA[@]}"
  fi
}

compositor_config() {
  local target defaults
  target=$(config_target)
  defaults=$(defaults_dir)
  [[ -d $defaults ]] || { echo "no se encontró la configuración de fábrica en $defaults."; return 1; }
  if [[ $CONFIG_ACTION == replace ]]; then
    local backup
    backup="${target}.bak-$(date +%Y%m%d-%H%M%S)"
    mv "$target" "$backup"
    echo "copia de seguridad en $backup"
  fi
  mkdir -p "$(dirname "$target")"
  cp -rv "$defaults" "$target"
}

default_apps() {
  xdg-mime default org.gnome.Nautilus.desktop inode/directory
  # En Niri, TERMINAL va en el bloque environment de config.kdl.
  if [[ $COMPOSITOR == hyprland ]]; then
    local env="${XDG_CONFIG_HOME:-$HOME/.config}/uwsm/env"
    mkdir -p "$(dirname "$env")"
    grep -qs '^export TERMINAL=' "$env" || echo 'export TERMINAL=kitty' >>"$env"
  fi
  systemctl --user daemon-reload
  systemctl --user enable okaru.service
}

greeter() {
  local conf=/etc/greetd/config.toml
  [[ -f $conf ]] && sudo cp -v "$conf" "${conf}.bak-$(date +%Y%m%d-%H%M%S)"
  printf '[terminal]\nvt = 1\n\n[default_session]\ncommand = "/usr/bin/okaru-greeter-session"\nuser = "greeter"\n' \
    | sudo tee "$conf"
  sudo /usr/share/okaru-greeter/setup_greetd_pam.sh
  [[ $SWITCH_DM == keep ]] && { echo "greetd queda instalado pero sin activar"; return 0; }
  [[ -n $SWITCH_DM ]] && sudo systemctl disable "$SWITCH_DM"
  sudo systemctl enable greetd.service
}

finish() {
  printf '\n'
  local login="Cierra sesión y entra a «Hyprland (uwsm-managed)»."
  local custom="Tus atajos propios van en ~/.config/hypr/oka/user-binds.lua"
  if [[ $COMPOSITOR == niri ]]; then
    login="Cierra sesión y entra a «Niri»."
    custom="Tus atajos propios van en ~/.config/niri/config.kdl (Super + Shift + / los muestra)"
  fi
  $WANT_GREETER && [[ $SWITCH_DM != keep ]] && login="Reinicia el equipo para entrar con OKARU Greeter."
  "$GUM" style --border rounded --border-foreground "$C_PRIMARY" --padding "1 3" \
    "$("$GUM" style --bold --foreground "$C_PRIMARY" "郁  OKARU Linux Shell instalada")" "" \
    "$login" "" \
    "Super + Espacio   lanzador" \
    "Super + Enter     terminal (kitty)" \
    "Super + E         archivos (Nautilus)" \
    "Super + ,         ajustes de OKARU" "" \
    "$custom"
  note "Registro: $LOG"
}

main() {
  checks
  bootstrap_gum
  # Con curl | bash la entrada estándar es el propio script: todo lo interactivo lee de la terminal.
  exec </dev/tty
  welcome
  choose_compositor
  choose_components
  authenticate
  title "Instalación"
  run_step "Repositorio [okaru] y su llave de firma" add_repo
  review
  title "Instalación"
  run_step "Paquetes" install_packages
  if [[ $CONFIG_ACTION == keep ]]; then
    ok "Configuración de $(compositor_name): se conserva la tuya (la de fábrica queda en $(defaults_dir))"
  else
    run_step "Configuración de fábrica de $(compositor_name)" compositor_config
  fi
  run_step "Apps por defecto y servicio de la shell" default_apps
  if $WANT_GREETER; then run_step "OKARU Greeter con greetd" greeter; fi
  finish
}

# En la misma línea: bash ya leyó todo el script antes de que main cambie la entrada a la terminal.
main "$@"; exit
