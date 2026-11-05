# Building OKARU from source

This guide covers source dependencies, distro-specific package installation, build modes, and install layouts.

## Dependencies

### Arch Linux / CachyOS

OKARU only supports Arch Linux and CachyOS. / OKARU solo da soporte a Arch Linux y CachyOS.

```sh
sudo pacman -S meson gcc just \
  wayland wayland-protocols \
  libglvnd freetype2 fontconfig \
  cairo pango harfbuzz \
  libxkbcommon glib2 \
  libsecret libsodium \
  sdbus-cpp libpipewire wireplumber polkit \
  pam curl libwebp libjxl libsndfile librsvg \
  libqalculate libxml2 \
  md4c tomlplusplus libical \
  nlohmann-json stb \
  jemalloc
```

Vendored dependencies, with no system package needed: `Wuffs`, `Luau`, `fzy`, and Material Color Utilities. Packagers who ship these separately can link them with `-Dsystem_libraries=true`.

System packages required beyond the Wayland/GL stack: `libwebp` handles WebP decoding and encoding (thumbnails, screenshots), `libjxl` handles JPEG XL decoding and screenshot encoding, `libsndfile` decodes shell sound effects (WAV, FLAC, Ogg/Vorbis, Opus, MP3, and AIFF), and Wuffs handles the other supported raster image formats. `libqalculate` powers the launcher calculator (arithmetic, unit and currency conversion).

HEIC, HEIF, and AVIF decoding is optional and needs no build-time package: OKARU loads `libheif.so.1` with `dlopen` the first time it meets such an image. Install `libheif` with its AV1 (dav1d or aom) and HEVC (libde265) decoder plugins to use those formats.

Polkit agent support uses the `polkit-agent-1` and `polkit-gobject-1` pkg-config modules, shipped in the `polkit` package.

PipeWire libraries/headers are sufficient to build OKARU, but the PipeWire daemon is required for audio features at runtime. OKARU remains running and reconnects in the background while the daemon is unavailable.

`upower` is an optional dependency used for battery and power device integration.

`ddcutil` is an optional dependency used for controlling monitor brightness.

Optional runtime tools used by the built-in modules and widgets. Each one is checked before use; without it only that feature is missing:

| Feature | Packages |
|---|---|
| Capture widget (screen recording) | `gpu-screen-recorder`, `slurp` (region) |
| vpn-manager | `networkmanager`; `nm-connection-editor` to edit connections in a window (falls back to `nmtui-edit`) |
| arch-updater | `pacman-contrib` (checkupdates, pacdiff); `flatpak`; `paru` or `yay` for AUR |
| phone-connect | `kdeconnect`, `sshfs` |
| claudecode | `claude-code`, `jq`, `curl` |
| cursor | `python`, `python-wand`, `imagemagick`, `win2xcur` (AUR); `flatpak` for Flatpak apps |
| icons | `python` |
| File dialogs in cursor, icons, phone-connect, vpn-manager | `zenity` (or `kdialog`) |
| ports widget | `iproute2` (`ss`, part of base) |

VPN credentials are asked in OKARU's own password dialog: the shell registers a NetworkManager secret agent and reads the fields to ask from the VPN plugin's auth dialog in external UI mode (supported by the fortisslvpn, openvpn and vpnc plugins).

Credential and encrypted-state persistence requires a Secret Service provider at runtime, such as GNOME Keyring, KWallet, or KeePassXC. `libsecret` is the client library and does not provide the session service by itself. OKARU continues to run when no provider is available, but features requiring durable secrets cannot persist them. CalDAV accounts may instead read their password from one explicitly configured regular file, which supports secret provisioners such as agenix and sops-nix without installing a Secret Service provider. Google refresh tokens and other writable credentials still require Secret Service. Encrypted state, including clipboard history and the calendar event cache, may instead read one storage master key from an explicitly configured file.

`jemalloc` is recommended but optional. It reduces memory fragmentation in long-running sessions, and on glibc systems it is used automatically when detected. Use Meson's `-Djemalloc=enabled` or `-Djemalloc=disabled` option to require or disable it explicitly.

Sanitizer runtime packages are only needed for ASan/UBSan builds configured with `just configure asan`.

The sources are built as C++23, which requires GCC 13+ or Clang 16+. Arch Linux and CachyOS ship a new enough compiler.

## Building and installing

Requires [just](https://github.com/casey/just) and [meson](https://mesonbuild.com/).

### Release build

```sh
# Optimized release build in build-release/
just configure release
just build release

# Install the selected build mode. This does not build or reconfigure.
sudo just install release
```

Release builds are portable by default. For a machine-local build, enable native CPU optimizations after configuring:

```sh
meson configure build-release -Dnative_optimizations=true
just build release
```

Pass a prefix to `configure` to install somewhere other than `/usr/local`:

```sh
just configure release "$HOME/.local"
just build release
just install release
```

To remove files installed from a build directory, run `just uninstall release`. The `install` and `uninstall` recipes require an explicit build mode so debug builds are not installed by accident.

### Debug build

```sh
# Debug build in build-debug/ for local development and troubleshooting.
just configure
just build

# Test your local debug build with
just run
```

Unit tests are not compiled by `just build`, which targets only the OKARU executable. Build and run them explicitly with `just test` (use `just test release` to force them on for a release build). Direct Meson users can control test target generation with the `-Dtests=enabled|disabled|auto` option. Production sources compile once into an internal static library shared by the shell and test executables.

Meson installs the binary and shipped assets using the normal prefix layout:

```text
/usr/local/bin/okaru
/usr/local/share/okaru/assets/...
```

OKARU needs the shipped `assets/` tree at runtime. Copying only the `okaru` binary is not enough.

Firefox theming uses the built-in template `post_action = "firefox-theme"` (same pattern as `kde-color-scheme`) plus the [Pywalfox](https://addons.mozilla.org/en-US/firefox/addon/pywalfox/) browser extension. Manual host helpers: `okaru firefox-theme --help`.

Portable bundle layouts are also supported:

```text
bundle/
  okaru
  assets/
```

```text
bundle/
  bin/okaru
  share/okaru/assets/
```

See [PACKAGING.md](PACKAGING.md#runtime-asset-lookup) for the full runtime asset lookup order.
