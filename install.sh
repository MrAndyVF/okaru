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
readonly COMMON_PACKAGES=(kitty nautilus xdg-desktop-portal-gtk noto-fonts ttf-jetbrains-mono-nerd fastfetch starship)
# Lo básico de cada compositor: sesión, portal de capturas/compartir pantalla y, en Niri, apps X11.
readonly HYPR_PACKAGES=(hyprland uwsm xdg-desktop-portal-hyprland)
readonly NIRI_PACKAGES=(niri xdg-desktop-portal-gnome xwayland-satellite)
readonly PHONE_PACKAGES=(kdeconnect sshfs)
# gum (la interfaz) se descarga a una carpeta temporal y se verifica; no queda instalado.
readonly GUM_VERSION="2.0.2"
readonly GUM_SHA256="d842e06d93dbed90af48cb8dd10698db6f22e331fc40346bb37bbc753109edc2"

# Idioma: español si el locale lo es, inglés si no. OKARU_LANG=es|en lo fuerza
# (curl -fsSL …/install.sh | OKARU_LANG=en bash).
LANG_TAG=${OKARU_LANG:-${LC_ALL:-${LC_MESSAGES:-${LANG:-}}}}
[[ $LANG_TAG == es* ]] && ES=true || ES=false
# t "español" "english" — imprime el texto del idioma activo.
t() { if $ES; then printf '%s' "$1"; else printf '%s' "$2"; fi; }

# Paleta OKARU
readonly C_PRIMARY="#7095C8" C_MUTED="#9AA4B2" C_OK="#7FA8C4" C_ERR="#D9707A" C_TEXT="#E6E9EF"

readonly OPT_HYPR="Hyprland ($(t "con" "with") uwsm)"
readonly OPT_NIRI="Niri"
readonly OPT_GREETER="OKARU Greeter ($(t "pantalla de inicio de sesión" "login screen"))"
readonly OPT_PHONE="$(t "Teléfono con KDE Connect" "Phone with KDE Connect")"
readonly OPT_EXTRA="$(t "Dependencias opcionales (grabación, VPN, actualizaciones…)" "Optional dependencies (recording, VPN, updates…)")"

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
  [[ $EUID -ne 0 ]] || plain_die "$(t "ejecútalo como tu usuario (pedirá la contraseña cuando haga falta), no como root." "run it as your user (it asks for the password when needed), not as root.")"
  command -v pacman >/dev/null || plain_die "$(t "este instalador es para Arch Linux y derivadas (no se encontró pacman)." "this installer is for Arch Linux and derivatives (pacman not found).")"
  [[ $(uname -m) == x86_64 ]] || plain_die "$(t "los paquetes de OKARU solo existen para x86_64." "OKARU packages only exist for x86_64.")"
  { : </dev/tty; } 2>/dev/null || plain_die "$(t "se necesita una terminal interactiva." "an interactive terminal is required.")"
  command -v sudo >/dev/null || plain_die "$(t "se necesita sudo." "sudo is required.")"
}

bootstrap_gum() {
  printf '\e[2m%s\e[0m\n' "$(t "Preparando el instalador…" "Preparing the installer…")"
  TMP=$(mktemp -d)
  local tarball="$TMP/gum.tar.gz"
  curl -fsSL "https://github.com/charmbracelet/gum/releases/download/v${GUM_VERSION}/gum_${GUM_VERSION}_Linux_x86_64.tar.gz" \
    -o "$tarball" || plain_die "$(t "no se pudo descargar gum (¿hay conexión a internet?)." "could not download gum (is there an internet connection?).")"
  echo "$GUM_SHA256  $tarball" | sha256sum -c --status || plain_die "$(t "gum descargado no coincide con su suma de verificación." "the downloaded gum does not match its checksum.")"
  tar -xzf "$tarball" -C "$TMP"
  GUM=$(find "$TMP" -type f -name gum -perm -u+x | head -n1)
  [[ -n $GUM ]] || plain_die "$(t "no se encontró gum dentro del archivo descargado." "gum was not found inside the downloaded archive.")"
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
    "$("$GUM" style --foreground "$C_MUTED" --italic --margin "1 0 0 1" "OKARU Linux Shell — $(t instalador installer)")")
  "$GUM" join --horizontal --align center "$kanji" "$name"
}

# Pantalla de error con las últimas líneas del registro; termina el instalador.
fail() {
  title "$("$GUM" style --foreground "$C_ERR" "$(t "La instalación falló" "Installation failed")")"
  "$GUM" style --foreground "$C_ERR" "$1"
  if [[ -s $LOG ]]; then
    printf '\n'
    note "$(t "Últimas líneas del registro:" "Last lines of the log:")"
    tail -n 15 "$LOG" | sed 's/^/  /'
    note "" "$(t "Registro completo" "Full log"): $LOG"
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
    fail "$(t "Falló" "Failed"): $label ($(t código code) $rc)"
  fi
  ok "$label"
}

# --- Pantallas ------------------------------------------------------------------------------------------

welcome() {
  banner
  local distro
  distro=$(. /etc/os-release 2>/dev/null && echo "${PRETTY_NAME:-Linux}")
  printf '\n'
  "$GUM" style --border rounded --border-foreground "$C_PRIMARY" --padding "0 2" "$(t Sistema System): $distro / $(uname -m)"
  title "$(t "QUÉ OBTIENES" "WHAT YOU GET")"
  if $ES; then
    "$GUM" style --foreground "$C_TEXT" \
      "  • OKARU Shell: barra, panel de control, lanzador, notificaciones y ajustes" \
      "  • Hyprland (con uwsm) o Niri, kitty como terminal y Nautilus como gestor de archivos" \
      "  • Reglas de ventana y atajos estándar de OKARU, sin configuración personal" \
      "  • fastfetch y starship con el estilo de OKARU" \
      "  • Paquetes firmados desde el repositorio [okaru]: se actualizan con pacman -Syu"
  else
    "$GUM" style --foreground "$C_TEXT" \
      "  • OKARU Shell: bar, control center, launcher, notifications and settings" \
      "  • Hyprland (with uwsm) or Niri, kitty as terminal and Nautilus as file manager" \
      "  • OKARU's standard window rules and shortcuts, nothing personal" \
      "  • fastfetch and starship in OKARU's style" \
      "  • Signed packages from the [okaru] repository: updated with pacman -Syu"
  fi
  printf '\n'
  confirm --affirmative "$(t Continuar Continue)" --negative "$(t Salir Quit)" "$(t "¿Empezamos?" "Shall we start?")" || exit 0
}

choose_compositor() {
  title "Compositor"
  note "$(t "OKARU funciona sobre los dos; cada uno se instala con lo básico para funcionar." "OKARU runs on both; each one is installed with the basics it needs.")"
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
  title "$(t Componentes Components)"
  note "$(t "Espacio marca o desmarca, Enter confirma." "Space toggles, Enter confirms.")"
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
  title "$(t "Permisos de administrador" "Administrator permissions")"
  if ! sudo -n true 2>/dev/null; then
    local tries pass
    for tries in 1 2 3; do
      pass=$("$GUM" input --password --placeholder "$(t "contraseña de $USER" "password for $USER")" --prompt "🔒 ") || exit 0
      printf '%s\n' "$pass" | sudo -S -p '' -v 2>/dev/null && break
      unset pass
      ((tries == 3)) && fail "$(t "No se pudo verificar la contraseña de sudo." "Could not verify the sudo password.")"
      "$GUM" style --foreground "$C_ERR" "$(t "Contraseña incorrecta, intenta de nuevo." "Wrong password, try again.")"
    done
    unset pass
  fi
  # Mantiene vivo el permiso de sudo mientras dura la instalación.
  (while sleep 50; do sudo -n -v 2>/dev/null || exit; done) &
  KEEPALIVE=$!
  ok "$(t "Permiso concedido" "Permission granted")"
}

add_repo() {
  local key="$TMP/okaru.asc"
  curl -fsSL "$KEY_URL" -o "$key"
  # La llave descargada debe ser exactamente la que está fijada en este script.
  if ! gpg --show-keys --with-colons "$key" 2>/dev/null | grep -q "^fpr:::::::::${KEY_FPR}:"; then
    t "la llave descargada no coincide con la huella esperada (${KEY_FPR})." "the downloaded key does not match the expected fingerprint (${KEY_FPR})."; echo
    return 1
  fi
  sudo pacman-key --add "$key"
  sudo pacman-key --lsign-key "$KEY_FPR"
  if grep -q '^\[okaru\]' /etc/pacman.conf; then
    t "[okaru] ya estaba en /etc/pacman.conf" "[okaru] was already in /etc/pacman.conf"; echo
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
  title "$(t "Revisión de paquetes" "Package review")"
  local pkg
  while read -r pkg; do
    if pacman -Q "$pkg" >/dev/null 2>&1; then
      printf '  %s %-30s %s\n' "$("$GUM" style --foreground "$C_OK" ✓)" "$pkg" "$(note "$(t "ya instalado" "already installed")")"
    else
      printf '  %s %-30s %s\n' "$("$GUM" style --foreground "$C_PRIMARY" +)" "$pkg" "$(note "$(t "se instalará" "will be installed")")"
    fi
  done < <(package_list)
  if $WANT_OPTIONAL; then
    mapfile -t EXTRA < <(optional_packages)
    ((${#EXTRA[@]})) && note "" "  $(t Opcionales Optional): ${EXTRA[*]}"
  fi
  note "" "  $(t "También se actualiza el sistema (pacman -Syu), como pide Arch al instalar paquetes." "The system is also updated (pacman -Syu), as Arch requires when installing packages.")"

  if $WANT_GREETER; then
    local current=""
    # Sin gestor de sesión, systemctl show devuelve igual «display-manager.service»: se mira el alias real.
    local link=/etc/systemd/system/display-manager.service
    [[ -L $link ]] && current=$(basename "$(readlink -f "$link")")
    if [[ -n $current && $current != greetd.service ]]; then
      printf '\n'
      if confirm --affirmative "$(t Cambiar Switch)" --negative "$(t Mantener Keep)" \
        "$(t "Ahora inicias sesión con ${current%.service}. ¿Cambiar a OKARU Greeter?" "You log in with ${current%.service} now. Switch to OKARU Greeter?")"; then
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
    title "$(t "Configuración de" "Configuration of") $(compositor_name)"
    note "$(t "Ya tienes ${target/#$HOME/\~}. Reemplazarla deja las reglas y atajos de OKARU; se guarda una copia con fecha." "You already have ${target/#$HOME/\~}. Replacing it leaves OKARU's rules and shortcuts; a dated backup is kept.")"
    local choice replace
    replace=$(t "Reemplazar por la de OKARU" "Replace with OKARU's")
    choice=$("$GUM" choose --cursor.foreground "$C_PRIMARY" "$(t "Conservar la mía" "Keep mine")" "$replace") || exit 0
    [[ $choice == "$replace" ]] && CONFIG_ACTION=replace || CONFIG_ACTION=keep
  fi

  printf '\n'
  confirm --affirmative "$(t Instalar Install)" --negative "$(t Cancelar Cancel)" "$(t "¿Instalar OKARU?" "Install OKARU?")" || exit 0
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
  [[ -d $defaults ]] || { t "no se encontró la configuración de fábrica en $defaults." "factory configuration not found in $defaults."; echo; return 1; }
  if [[ $CONFIG_ACTION == replace ]]; then
    local backup
    backup="${target}.bak-$(date +%Y%m%d-%H%M%S)"
    mv "$target" "$backup"
    t "copia de seguridad en $backup" "backup in $backup"; echo
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

# JetBrainsMono Nerd Font en todo el sistema: fontconfig (interfaz, serif y monoespaciada), GTK y kitty.
# No pisa nada propio: el archivo de fontconfig solo se copia si no existe y kitty solo si no fija font_family.
system_font() {
  local config="${XDG_CONFIG_HOME:-$HOME/.config}"
  local fc="$config/fontconfig/conf.d/50-okaru-font.conf"
  if [[ ! -e $fc ]]; then
    mkdir -p "$(dirname "$fc")"
    cp -v "$DEFAULTS_ROOT/fontconfig/50-okaru-font.conf" "$fc"
  fi
  fc-cache -f >/dev/null 2>&1 || true
  gsettings set org.gnome.desktop.interface font-name 'JetBrainsMono Nerd Font 11' || true
  gsettings set org.gnome.desktop.interface monospace-font-name 'JetBrainsMono Nerd Font Mono 11' || true
  local kitty="$config/kitty/kitty.conf"
  if ! grep -qs '^[[:space:]]*font_family' "$kitty"; then
    mkdir -p "$(dirname "$kitty")"
    printf '\n# OKARU\nfont_family JetBrainsMono Nerd Font Mono\n' >>"$kitty"
  fi
}

# fastfetch (basado en el de Omarchy) y starship con el estilo de OKARU. Solo se copian si no existen.
terminal() {
  local config="${XDG_CONFIG_HOME:-$HOME/.config}"
  if [[ ! -e $config/fastfetch/config.jsonc ]]; then
    mkdir -p "$config/fastfetch"
    cp -v "$DEFAULTS_ROOT"/fastfetch/* "$config/fastfetch/"
  fi
  [[ -e $config/starship.toml ]] || cp -v "$DEFAULTS_ROOT/starship.toml" "$config/starship.toml"
  starship_init "$(getent passwd "$USER" | cut -d: -f7)" "$config" "${ZDOTDIR:-$HOME}" "$HOME"
}

# Agrega `starship init` al final del archivo del shell de inicio de sesión, si no lo tiene ya.
# CachyOS carga su config desde /usr/share/cachyos-{fish,zsh}-config con un source en ese archivo: ir al final
# hace que starship se cargue después y no lo pise. En zsh esa config trae powerlevel10k, que se descarga
# antes del primer prompt (así tampoco abre su asistente). No cuenta un init escrito en /usr/share: pacman lo
# restaura al actualizar el paquete.
# Uso: starship_init <shell> <XDG_CONFIG_HOME> <ZDOTDIR> <HOME>
starship_init() {
  local rc line
  case ${1##*/} in
    fish)
      rc="$2/fish/config.fish"
      line='command -q starship; and starship init fish | source'
      ;;
    zsh)
      rc="$3/.zshrc"
      line='(( $+functions[powerlevel10k_plugin_unload] )) && powerlevel10k_plugin_unload
(( $+commands[starship] )) && eval "$(starship init zsh)"'
      ;;
    bash)
      rc="$4/.bashrc"
      line='command -v starship >/dev/null && eval "$(starship init bash)"'
      ;;
    *) t "shell ${1##*/} sin soporte: agrega starship a mano (https://starship.rs)" "unsupported shell ${1##*/}: add starship by hand (https://starship.rs)"; echo; return 0 ;;
  esac
  grep -qs 'starship init' "$rc" && return 0
  mkdir -p "$(dirname "$rc")"
  printf '\n# OKARU: prompt con starship (al final, para cargarse después de la config de CachyOS)\n%s\n' "$line" >>"$rc"
  t "starship agregado a $rc" "starship added to $rc"; echo
}

greeter() {
  local conf=/etc/greetd/config.toml
  [[ -f $conf ]] && sudo cp -v "$conf" "${conf}.bak-$(date +%Y%m%d-%H%M%S)"
  printf '[terminal]\nvt = 1\n\n[default_session]\ncommand = "/usr/bin/okaru-greeter-session"\nuser = "greeter"\n' \
    | sudo tee "$conf"
  sudo /usr/share/okaru-greeter/setup_greetd_pam.sh
  [[ $SWITCH_DM == keep ]] && { t "greetd queda instalado pero sin activar" "greetd is installed but not enabled"; echo; return 0; }
  [[ -n $SWITCH_DM ]] && sudo systemctl disable "$SWITCH_DM"
  sudo systemctl enable greetd.service
}

finish() {
  printf '\n'
  local login custom
  login=$(t "Cierra sesión y entra a «Hyprland (uwsm-managed)»." "Log out and choose «Hyprland (uwsm-managed)».")
  custom=$(t "Tus atajos propios van en ~/.config/hypr/oka/user-binds.lua" "Your own shortcuts go in ~/.config/hypr/oka/user-binds.lua")
  if [[ $COMPOSITOR == niri ]]; then
    login=$(t "Cierra sesión y entra a «Niri»." "Log out and choose «Niri».")
    custom=$(t "Tus atajos propios van en ~/.config/niri/config.kdl (Super + Shift + / los muestra)" "Your own shortcuts go in ~/.config/niri/config.kdl (Super + Shift + / shows them)")
  fi
  $WANT_GREETER && [[ $SWITCH_DM != keep ]] && login=$(t "Reinicia el equipo para entrar con OKARU Greeter." "Reboot to log in with OKARU Greeter.")
  "$GUM" style --border rounded --border-foreground "$C_PRIMARY" --padding "1 3" \
    "$("$GUM" style --bold --foreground "$C_PRIMARY" "郁  OKARU Linux Shell $(t instalada installed)")" "" \
    "$login" "" \
    "$(t "Super + Espacio   lanzador" "Super + Space     launcher")" \
    "Super + Enter     terminal (kitty)" \
    "$(t "Super + E         archivos (Nautilus)" "Super + E         files (Nautilus)")" \
    "$(t "Super + ,         ajustes de OKARU" "Super + ,         OKARU settings")" "" \
    "$custom"
  note "$(t Registro Log): $LOG"
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
  title "$(t Instalación Installation)"
  run_step "$(t "Repositorio [okaru] y su llave de firma" "[okaru] repository and its signing key")" add_repo
  review
  title "$(t Instalación Installation)"
  run_step "$(t Paquetes Packages)" install_packages
  if [[ $CONFIG_ACTION == keep ]]; then
    ok "$(t "Configuración de $(compositor_name): se conserva la tuya (la de fábrica queda en $(defaults_dir))" "$(compositor_name) configuration: yours is kept (the factory one stays in $(defaults_dir))")"
  else
    run_step "$(t "Configuración de fábrica de $(compositor_name)" "$(compositor_name) factory configuration")" compositor_config
  fi
  run_step "$(t "Apps por defecto y servicio de la shell" "Default apps and shell service")" default_apps
  run_step "$(t "Fuente del sistema (JetBrainsMono Nerd Font)" "System font (JetBrainsMono Nerd Font)")" system_font
  run_step "$(t "Terminal: fastfetch y starship" "Terminal: fastfetch and starship")" terminal
  if $WANT_GREETER; then run_step "$(t "OKARU Greeter con greetd" "OKARU Greeter with greetd")" greeter; fi
  finish
}

# En la misma línea: bash ya leyó todo el script antes de que main cambie la entrada a la terminal.
main "$@"; exit
