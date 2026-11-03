# Phone Connect

Control your KDE Connect-paired phone from the OKARU bar.

## Plugin

| Entry | Type | Id |
|-------|------|----|
| `okaru/phone-connect` (integrado en OKARU; original `icefish/phone-connect`) | plugin | — |
| `bar` | widget | `okaru/phone-connect:bar` |
| `tile` | shortcut | `okaru/phone-connect:tile` |
| `details` | panel | `okaru/phone-connect:details` |
| `details_floating` | panel | `okaru/phone-connect:details_floating` |
| `details_widget` | panel | `okaru/phone-connect:details_widget` |
| `kde` | service | `okaru/phone-connect:kde` |

## Usage

Add the bar widget and control-center tile from Settings, then click either to open the details panel.

Open panels from the shell:

```
okaru msg panel-toggle okaru/phone-connect:details
okaru msg panel-toggle okaru/phone-connect:details_floating
okaru msg panel-toggle okaru/phone-connect:details_widget
```

Send IPC events to the service:

```
okaru msg plugin okaru/phone-connect:kde all refresh
okaru msg plugin okaru/phone-connect:kde all cmd '{"op":"ring","device":"<id>"}'
```

## Features

- **Device status** — battery level, charging state, network type (5G/LTE), pairing
- **Quick actions** — ring, ping, send clipboard, share text/files/URLs, SMS, SFTP browser
- **Media control** — play/pause/skip/seek, album art, now-playing (MPRIS)
- **Customization** — configurable bar contents, per-device image and alias, three panel placements
- **Language** — English / Simplified Chinese

## Requirements

- `kdeconnect` — KDE Connect daemon and CLI
- `gdbus` — ships with glib2
- `sshfs` — optional, for phone file browsing

## Settings

| Setting | Default | Description |
|---------|---------|-------------|
| State Update Interval | 30 | Seconds between device refreshes |
| Show Charging Fill | true | Highlight bar widget when charging |
| Show Device Name | false | Show the selected device's name or alias |
| Battery Display | icon_and_percent | Show the battery icon, percentage, both, or neither |
| Show Clipboard Action | true | Show clipboard button in panel |
| Device Image | — | Custom image for selected device |
| Device Alias | Your-Phone | Custom display name |
| Language | en | English / 简体中文 |
| Panel Placement | attached | Attached / Floating center / Below widget |

## IPC

| Event | Payload | Description |
|-------|---------|-------------|
| `refresh` | — | Full device refresh |
| `cmd` | JSON `{"op":"…","device":"…"}` | Execute an operation |

Supported ops: `ring`, `ping`, `clipboard`, `share_text`, `share_url`, `share_file`, `pair`, `accept`, `reject`, `unpair`, `browse`, `sms_send`, `launch_sms_app`, `media`, `media_seek`, `select`, `refresh`, `set_image`.

## Notes

- Communicates with KDE Connect via DBus (`gdbus call`) and CLI (`kdeconnect-cli`)
- Persists user preferences to `pluginDataDir()/state.json`
- No external network requests; all logic runs locally

## Development tests

The mocked regression checks run the plugin's Lua-compatible code with Lua 5.4,
without OKARU, a phone, filesystem writes, or spawned commands:

```sh
lua phone-connect/tests/regressions.lua phone-connect service
lua phone-connect/tests/regressions.lua phone-connect panel
```

They cover a connected phone without a cellular report and device cards with
missing, present, and offline media volume.
