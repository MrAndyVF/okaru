#!/usr/bin/env bash
# OKARU: aplica la fuente de la shell (familia + peso) al resto del sistema.
# Uso: font-sync.sh "<familia>" <peso CSS 100-900>
# La lanza OKARU al cambiar shell.font_family o shell.font_weight (si shell.sync_system_font).
# Toca solo lo que existe: GTK (gsettings), qt5ct/qt6ct, kitty (recarga las ventanas abiertas;
# starship y demás apps de terminal usan la fuente de la terminal), VS Code, Zed y ghostty.
set -u

family=${1:?familia}
weight=${2:-400}
config="${XDG_CONFIG_HOME:-$HOME/.config}"

# Peso CSS -> palabra de fontconfig.
case $weight in
  100) keyword=thin ;; 200) keyword=extralight ;; 300) keyword=light ;; 350) keyword=semilight ;;
  380) keyword=book ;; 500) keyword=medium ;; 600) keyword=semibold ;; 700) keyword=bold ;;
  800) keyword=extrabold ;; 900) keyword=black ;; *) keyword=regular weight=400 ;;
esac

# Variante monoespaciada para terminales y editores: "<familia> Mono" si existe (Nerd Fonts).
mono=$family
if [[ $family != *Mono ]] && fc-list : family | tr ',' '\n' | grep -qxF "$family Mono"; then
  mono="$family Mono"
fi

# Nombre real del estilo (p. ej. "SemiBold"); "Regular" si la fuente no tiene ese peso.
style_of() {
  local fam=$1 got style
  got=$(fc-match -f '%{family[0]}' "$fam:$keyword")
  style=$(fc-match -f '%{style[0]}' "$fam:$keyword")
  [[ $got == "$fam" && -n $style ]] && echo "$style" || echo Regular
}
style=$(style_of "$family")
mono_style=$(style_of "$mono")

# --- GTK / gsettings (conserva el tamaño actual) ---
gtk_font() {
  local key=$1 fam=$2 sty=$3 size
  size=$(gsettings get org.gnome.desktop.interface "$key" 2>/dev/null | tr -d "'" | grep -oE '[0-9.]+$')
  [[ $sty == Regular ]] && sty="" || sty=" $sty"
  gsettings set org.gnome.desktop.interface "$key" "$fam$sty ${size:-11}" 2>/dev/null || true
}
if command -v gsettings >/dev/null; then
  gtk_font font-name "$family" "$style"
  gtk_font monospace-font-name "$mono" "$mono_style"
fi

# --- Qt (qt5ct / qt6ct): Qt 6 usa el peso CSS; Qt 5, su escala 0-99 ---
qt5_weight() {
  case $1 in 100) echo 0 ;; 200) echo 12 ;; 300) echo 25 ;; 500) echo 57 ;; 600) echo 63 ;;
    700) echo 75 ;; 800) echo 81 ;; 900) echo 87 ;; *) echo 50 ;; esac
}
qt_fonts() {
  local file=$1 general=$2 fixed=$3
  [[ -f $file ]] || return 0
  if grep -q '^\[Fonts\]' "$file"; then
    sed -i '/^\[Fonts\]/,/^\[/{/^general=/d;/^fixed=/d}' "$file"
    sed -i "/^\[Fonts\]/a general=\"$general\"\nfixed=\"$fixed\"" "$file"
  else
    printf '\n[Fonts]\ngeneral="%s"\nfixed="%s"\n' "$general" "$fixed" >>"$file"
  fi
}
qt_fonts "$config/qt6ct/qt6ct.conf" "$family,11,-1,5,$weight,0,0,0,0,0,0,0,0,0,0,1" \
  "$mono,11,-1,5,$weight,0,0,0,0,0,0,0,0,0,0,1"
w5=$(qt5_weight "$weight")
qt_fonts "$config/qt5ct/qt5ct.conf" "$family,11,-1,5,$w5,0,0,0,0,0" "$mono,11,-1,5,$w5,0,0,0,0,0"

# --- kitty: bloque de fuentes propio y recarga en caliente (SIGUSR1) ---
kitty_conf="$config/kitty/kitty.conf"
if [[ -f $kitty_conf ]]; then
  sed -i '/^# BEGIN_KITTY_FONTS/,/^# END_KITTY_FONTS/d;/^[[:space:]]*\(font_family\|bold_font\|italic_font\|bold_italic_font\)[[:space:]]/d' "$kitty_conf"
  printf '# BEGIN_KITTY_FONTS\nfont_family      family='"'"'%s'"'"' style=%s\nbold_font        auto\nitalic_font      auto\nbold_italic_font auto\n# END_KITTY_FONTS\n' \
    "$mono" "$mono_style" >>"$kitty_conf"
  pkill -USR1 -x kitty 2>/dev/null || true
fi

# --- VS Code (editor y terminal integrada) ---
json_set() { # archivo clave valor-json: reemplaza la línea de la clave o la agrega tras la primera «{»
  local file=$1 key=$2 value=$3
  if grep -q "\"$key\"[[:space:]]*:" "$file"; then
    # Reemplaza todo el valor (puede llevar comas); la coma final solo si la línea ya la tenía.
    sed -i -E "/\"$key\"[[:space:]]*:/{/,[[:space:]]*\$/{s|(\"$key\"[[:space:]]*:).*|\\1 $value,|;b};s|(\"$key\"[[:space:]]*:).*|\\1 $value|}" "$file"
  else
    sed -i "0,/{/s|{|{\n  \"$key\": $value,|" "$file"
  fi
}
vscode="$config/Code/User/settings.json"
if [[ -f $vscode ]]; then
  json_set "$vscode" editor.fontFamily "\"'$mono', monospace\""
  json_set "$vscode" editor.fontWeight "\"$weight\""
  json_set "$vscode" terminal.integrated.fontFamily "\"'$mono', monospace\""
  json_set "$vscode" terminal.integrated.fontWeight "\"$weight\""
fi

# --- Zed (editor y terminal) ---
zed="$config/zed/settings.json"
if [[ -f $zed ]]; then
  json_set "$zed" buffer_font_family "\"$mono\""
  json_set "$zed" buffer_font_weight "$weight"
  if grep -q '"terminal"[[:space:]]*:[[:space:]]*{[^}]*}' "$zed"; then
    sed -i "s|\"terminal\"[[:space:]]*:[[:space:]]*{[^}]*}|\"terminal\": { \"font_family\": \"$mono\", \"font_weight\": $weight }|" "$zed"
  fi
fi

# --- ghostty ---
for ghostty in "$config/ghostty/config.ghostty" "$config/ghostty/config"; do
  [[ -f $ghostty ]] || continue
  sed -i '/^[[:space:]]*font-family[[:space:]]*=/d;/^[[:space:]]*font-style[[:space:]]*=/d' "$ghostty"
  printf 'font-family = %s\nfont-style = %s\n' "$mono" "$mono_style" >>"$ghostty"
  break
done

exit 0
