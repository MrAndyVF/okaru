-- Atajos de fábrica de OKARU. Administrado por OKARU (Ajustes → Atajos).
-- Para cambiar o agregar los tuyos usa oka/user-binds.lua.
local ipc = "okaru msg "

-- === Aplicaciones de fábrica (install.sh instala kitty y Nautilus) ===
hl.bind("SUPER + Return", hl.dsp.exec_cmd("kitty"))
hl.bind("SUPER + E", hl.dsp.exec_cmd("nautilus --new-window"))

-- === OKARU ===
hl.bind("SUPER + space", hl.dsp.exec_cmd(ipc .. "panel-toggle launcher"))
hl.bind("SUPER + Period", hl.dsp.exec_cmd(ipc .. [[panel-toggle launcher "/emo "]]), { description = "Emoji Picker" })
hl.bind("SUPER + V", hl.dsp.exec_cmd(ipc .. "panel-toggle clipboard"))
hl.bind("SUPER + comma", hl.dsp.exec_cmd(ipc .. "settings-toggle"))
hl.bind("SUPER + A", hl.dsp.exec_cmd(ipc .. "panel-toggle control-center"))
hl.bind("SUPER + N", hl.dsp.exec_cmd(ipc .. "panel-toggle okaru-notifications"))
hl.bind("SUPER + D", hl.dsp.exec_cmd(ipc .. "panel-toggle okaru-media"))
hl.bind("SUPER + Y", hl.dsp.exec_cmd(ipc .. "panel-toggle wallpaper"))
hl.bind("SUPER + X", hl.dsp.exec_cmd(ipc .. "panel-toggle session"))
hl.bind("SUPER + ALT + N", hl.dsp.exec_cmd(ipc .. "nightlight-force-toggle"))
hl.bind("SUPER + ALT + I", hl.dsp.exec_cmd(ipc .. "caffeine-toggle"))
hl.bind("ALT + Tab", hl.dsp.exec_cmd(ipc .. "window-switcher hold"))
hl.bind("ALT + SHIFT + Tab", hl.dsp.exec_cmd(ipc .. "window-switcher hold"))

-- === Sesión ===
hl.bind("SUPER + ALT + L", hl.dsp.exec_cmd(ipc .. "session lock"))
hl.bind("SUPER + SHIFT + E", hl.dsp.exit())
hl.bind("SUPER + SHIFT + P", hl.dsp.dpms({ action = "toggle" }))

-- === Audio, multimedia y brillo ===
hl.bind("XF86AudioRaiseVolume", hl.dsp.exec_cmd(ipc .. "volume-up 5"), { locked = true, repeating = true })
hl.bind("XF86AudioLowerVolume", hl.dsp.exec_cmd(ipc .. "volume-down 5"), { locked = true, repeating = true })
hl.bind("XF86AudioMute", hl.dsp.exec_cmd(ipc .. "volume-mute"), { locked = true })
hl.bind("XF86AudioMicMute", hl.dsp.exec_cmd(ipc .. "mic-mute"), { locked = true })
hl.bind("XF86AudioPlay", hl.dsp.exec_cmd(ipc .. "media toggle"), { locked = true })
hl.bind("XF86AudioPause", hl.dsp.exec_cmd(ipc .. "media toggle"), { locked = true })
hl.bind("XF86AudioPrev", hl.dsp.exec_cmd(ipc .. "media previous"), { locked = true })
hl.bind("XF86AudioNext", hl.dsp.exec_cmd(ipc .. "media next"), { locked = true })
hl.bind("XF86MonBrightnessUp", hl.dsp.exec_cmd(ipc .. "brightness-up"), { locked = true, repeating = true })
hl.bind("XF86MonBrightnessDown", hl.dsp.exec_cmd(ipc .. "brightness-down"), { locked = true, repeating = true })

-- === Capturas ===
hl.bind("Print", hl.dsp.exec_cmd(ipc .. "screenshot-region --annotate"))
hl.bind("CTRL + Print", hl.dsp.exec_cmd(ipc .. "screenshot-fullscreen"))
hl.bind("ALT + Print", hl.dsp.exec_cmd(ipc .. "screenshot-window"))

-- === Ventanas ===
hl.bind("SUPER + Q", hl.dsp.window.close())
hl.bind("SUPER + F", hl.dsp.window.fullscreen({ mode = "maximized", action = "toggle" }))
hl.bind("SUPER + SHIFT + F", hl.dsp.window.fullscreen({ mode = "fullscreen", action = "toggle" }))
hl.bind("SUPER + SHIFT + T", hl.dsp.window.float({ action = "toggle" }))
hl.bind("SUPER + W", hl.dsp.group.toggle())
hl.bind("SUPER + mouse:272", hl.dsp.window.drag(), { mouse = true, description = "Move window" })
hl.bind("SUPER + mouse:273", hl.dsp.window.resize(), { mouse = true, description = "Resize window" })

-- === Foco y mover (flechas o H/J/K/L) ===
for key, dir in pairs({ left = "l", down = "d", up = "u", right = "r", H = "l", J = "d", K = "u", L = "r" }) do
	hl.bind("SUPER + " .. key, hl.dsp.focus({ direction = dir }))
	hl.bind("SUPER + SHIFT + " .. key, hl.dsp.window.move({ direction = dir }))
	hl.bind("SUPER + CTRL + " .. key, hl.dsp.focus({ monitor = dir }))
	hl.bind("SUPER + SHIFT + CTRL + " .. key, hl.dsp.window.move({ monitor = dir }))
end

-- === Workspaces ===
for n = 1, 9 do
	hl.bind("SUPER + " .. n, hl.dsp.focus({ workspace = n }))
	hl.bind("SUPER + SHIFT + " .. n, hl.dsp.window.move({ workspace = n }))
end
hl.gesture({ fingers = 3, direction = "horizontal", action = "workspace" })

-- === Scratchpad ===
hl.bind("SUPER + S", hl.dsp.workspace.toggle_special())
hl.bind("SUPER + SHIFT + S", hl.dsp.window.move({ workspace = "special", follow = false }))
