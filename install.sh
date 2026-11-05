#!/usr/bin/env bash
# Instalador de OKARU (郁) para Arch Linux y derivadas, sin compilar nada.
#
#   curl -fsSL https://raw.githubusercontent.com/MrAndyVF/okaru/main/install.sh | bash
#
# Agrega el repositorio pacman [okaru] (paquetes firmados), instala la shell con Hyprland, kitty y Nautilus,
# copia la configuración de fábrica de Hyprland (reglas y atajos estándar, nada personal) y, si quieres,
# el greeter, el panel del teléfono y las dependencias opcionales.
set -euo pipefail

readonly REPO_URL="https://github.com/MrAndyVF/okaru/releases/download/repo"
readonly KEY_URL="https://raw.githubusercontent.com/MrAndyVF/okaru/main/packaging/keys/okaru.asc"
# Huella de la llave que firma los paquetes; debe coincidir con packaging/keys/okaru.asc.
readonly KEY_FPR="PENDIENTE"
readonly DEFAULTS="/usr/share/okaru/defaults/hypr"
readonly BASE_PACKAGES=(hyprland uwsm kitty nautilus xdg-desktop-portal-hyprland xdg-desktop-portal-gtk)
readonly PHONE_PACKAGES=(kdeconnect sshfs)

bold=$'\e[1m' blue=$'\e[34m' red=$'\e[31m' dim=$'\e[2m' reset=$'\e[0m'
say() { printf '%s==>%s %s\n' "$blue$bold" "$reset" "$*"; }
info() { printf '    %s%s%s\n' "$dim" "$*" "$reset"; }
die() { printf '%serror:%s %s\n' "$red$bold" "$reset" "$*" >&2; exit 1; }

# Las respuestas se leen de la terminal: con curl | bash la entrada estándar es el propio script.
ask() { # ask "pregunta" s|n -> 0 si sí
  local prompt=$1 default=$2 hint answer
  [[ $default == s ]] && hint="S/n" || hint="s/N"
  read -r -p "$(printf '%s?%s %s [%s] ' "$bold" "$reset" "$prompt" "$hint")" answer </dev/tty || answer=""
  answer=${answer:-$default}
  [[ ${answer,,} == s* || ${answer,,} == y* ]]
}

checks() {
  [[ $EUID -ne 0 ]] || die "ejecútalo como tu usuario (pedirá sudo cuando haga falta), no como root."
  command -v pacman >/dev/null || die "este instalador es para Arch Linux y derivadas (no se encontró pacman)."
  [[ -r /dev/tty ]] || die "se necesita una terminal interactiva."
  command -v sudo >/dev/null || die "se necesita sudo."
  [[ $KEY_FPR != PENDIENTE ]] || die "el instalador aún no tiene la llave de firma configurada."
}

add_repo() {
  say "Repositorio [okaru] y su llave de firma"
  local key
  key=$(mktemp)
  curl -fsSL "$KEY_URL" -o "$key"
  # La llave descargada debe ser exactamente la que está fijada en este script.
  gpg --show-keys --with-colons "$key" 2>/dev/null | grep -q "^fpr:::::::::${KEY_FPR}:" \
    || die "la llave descargada no coincide con la huella esperada (${KEY_FPR})."
  sudo pacman-key --add "$key" >/dev/null
  sudo pacman-key --lsign-key "$KEY_FPR" >/dev/null
  rm -f "$key"
  if grep -q '^\[okaru\]' /etc/pacman.conf; then
    info "[okaru] ya estaba en /etc/pacman.conf"
  else
    printf '\n[okaru]\nServer = %s\n' "$REPO_URL" | sudo tee -a /etc/pacman.conf >/dev/null
    info "agregado [okaru] a /etc/pacman.conf"
  fi
}

optional_packages() {
  # Opcionales de okaru que existen en los repos oficiales (las de AUR o de paquetes propios no pasan el filtro).
  pacman -Si okaru 2>/dev/null | awk '
    /^Optional Deps/ { on = 1; sub(/^Optional Deps *: */, "") }
    on && /^[A-Z][A-Za-z ]* *: / && !/^Optional Deps/ { exit }
    on { sub(/^ +/, ""); split($0, parts, ":"); if (parts[1] != "None") print parts[1] }
  ' | while read -r name; do
    pacman -Si "$name" >/dev/null 2>&1 && printf '%s\n' "$name"
  done
}

install_packages() {
  local packages=("${BASE_PACKAGES[@]}" okaru)
  $WANT_PHONE && packages+=("${PHONE_PACKAGES[@]}")
  $WANT_GREETER && packages+=(greetd okaru-greeter)
  say "Instalando: ${packages[*]}"
  sudo pacman -Syu --needed "${packages[@]}"
  if $WANT_OPTIONAL; then
    mapfile -t extra < <(LC_ALL=C optional_packages)
    if ((${#extra[@]})); then
      say "Dependencias opcionales: ${extra[*]}"
      sudo pacman -S --needed --asdeps "${extra[@]}"
    fi
  fi
}

hypr_config() {
  local target="${XDG_CONFIG_HOME:-$HOME/.config}/hypr"
  [[ -d $DEFAULTS ]] || die "no se encontró la configuración de fábrica en $DEFAULTS."
  if [[ -e $target ]]; then
    if ask "Ya tienes configuración de Hyprland. ¿Reemplazarla por la de OKARU? (se guarda una copia)" n; then
      local backup="${target}.bak-$(date +%Y%m%d-%H%M%S)"
      mv "$target" "$backup"
      info "copia de seguridad en $backup"
    else
      info "se deja tu configuración; la de fábrica queda en $DEFAULTS"
      return
    fi
  fi
  say "Configuración de fábrica de Hyprland en $target"
  mkdir -p "$(dirname "$target")"
  cp -r "$DEFAULTS" "$target"
}

default_apps() {
  say "Apps por defecto: Nautilus (archivos) y kitty (terminal)"
  xdg-mime default org.gnome.Nautilus.desktop inode/directory
  local env="${XDG_CONFIG_HOME:-$HOME/.config}/uwsm/env"
  mkdir -p "$(dirname "$env")"
  grep -qs '^export TERMINAL=' "$env" || echo 'export TERMINAL=kitty' >>"$env"
}

shell_service() {
  say "Servicio de la shell (okaru.service, arranca con la sesión de uwsm)"
  systemctl --user daemon-reload
  systemctl --user enable okaru.service >/dev/null
}

greeter() {
  say "OKARU Greeter con greetd"
  local conf=/etc/greetd/config.toml
  [[ -f $conf ]] && sudo cp "$conf" "${conf}.bak-$(date +%Y%m%d-%H%M%S)"
  printf '[terminal]\nvt = 1\n\n[default_session]\ncommand = "/usr/bin/okaru-greeter-session"\nuser = "greeter"\n' \
    | sudo tee "$conf" >/dev/null
  sudo /usr/share/okaru-greeter/setup_greetd_pam.sh
  local current
  current=$(systemctl show display-manager.service -p Id --value 2>/dev/null || true)
  if [[ -n $current && $current != greetd.service ]]; then
    if ask "Ahora inicias sesión con ${current%.service}. ¿Cambiar a OKARU Greeter?" s; then
      sudo systemctl disable "$current" >/dev/null
    else
      info "greetd queda instalado pero sin activar"
      return
    fi
  fi
  sudo systemctl enable greetd.service >/dev/null
}

main() {
  printf '\n%sOKARU (郁)%s — instalador\n\n' "$bold" "$reset"
  checks
  WANT_GREETER=false WANT_PHONE=false WANT_OPTIONAL=false
  ask "¿Instalar OKARU Greeter como pantalla de inicio de sesión?" s && WANT_GREETER=true
  ask "¿Instalar el soporte del teléfono (KDE Connect)?" s && WANT_PHONE=true
  ask "¿Instalar también las dependencias opcionales (grabación, VPN, actualizaciones...)?" n && WANT_OPTIONAL=true

  add_repo
  install_packages
  hypr_config
  default_apps
  shell_service
  $WANT_GREETER && greeter

  printf '\n%sListo.%s Cierra sesión y entra a «Hyprland (uwsm-managed)».\n' "$bold" "$reset"
  printf 'Tus atajos propios van en ~/.config/hypr/oka/user-binds.lua.\n'
}

main "$@"
