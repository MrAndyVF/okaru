-- Atajos de fábrica de OKARU. Administrado por OKARU (Ajustes → Atajos).
-- Para cambiar o agregar los tuyos usa oka/user-binds.lua.
local ipc = "okaru msg "

-- Atajos que dependen del layout del workspace activo (scrolling, dwindle, master, monocle).
local function active_layout()
	local ws = hl.get_active_workspace()
	return ws and ws.tiled_layout or ""
end

-- Despacha la acción del layout activo (los que no están en la tabla no hacen nada).
local function by_layout(actions)
	return function()
		local action = actions[active_layout()]
		if action then hl.dispatch(action) end
	end
end

-- ¿Hay otra columna hacia `dir` ("l"/"r")? En el borde, el foco pasa al otro monitor como siempre.
local function column_beyond(w, dir)
	for _, o in ipairs(hl.get_workspace_windows(w.workspace)) do
		if not o.floating and o.address ~= w.address and ((dir == "l" and o.at.x < w.at.x) or (dir == "r" and o.at.x > w.at.x)) then
			return true
		end
	end
	return false
end

-- Scrolling usa su propio foco: el general no ve las columnas tapadas por una ventana maximizada.
local function focus(dir)
	return function()
		local w = hl.get_active_window()
		local tiled = w and not w.floating
		local layout = active_layout()
		if layout == "scrolling" and tiled and (dir == "u" or dir == "d" or column_beyond(w, dir)) then
			hl.dispatch(hl.dsp.layout("focus " .. dir))
		elseif layout == "monocle" and tiled then
			hl.dispatch(hl.dsp.layout((dir == "l" or dir == "u") and "cycleprev" or "cyclenext"))
		else
			hl.dispatch(hl.dsp.focus({ direction = dir }))
		end
	end
end

-- En scrolling, izquierda/derecha intercambia columnas; en los demás, mueve la ventana.
local function move(dir)
	return function()
		local w = hl.get_active_window()
		if active_layout() == "scrolling" and w and not w.floating and (dir == "l" or dir == "r") then
			hl.dispatch(hl.dsp.layout("swapcol " .. dir))
		else
			hl.dispatch(hl.dsp.window.move({ direction = dir }))
		end
	end
end

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
	hl.bind("SUPER + " .. key, focus(dir))
	hl.bind("SUPER + SHIFT + " .. key, move(dir))
	hl.bind("SUPER + CTRL + " .. key, hl.dsp.focus({ monitor = dir }))
	hl.bind("SUPER + SHIFT + CTRL + " .. key, hl.dsp.window.move({ monitor = dir }))
end

-- === Layouts (alcance en Ajustes → Ventanas → Distribución) ===
hl.bind("SUPER + ALT + space", hl.dsp.exec_cmd(ipc .. "hypr-layout next"), { description = "Next layout" })
hl.bind("SUPER + ALT + SHIFT + space", hl.dsp.exec_cmd(ipc .. "hypr-layout prev"), { description = "Previous layout" })
hl.bind("SUPER + R", by_layout({ scrolling = hl.dsp.layout("colresize +conf"), dwindle = hl.dsp.layout("togglesplit"), master = hl.dsp.layout("orientationnext") }))
hl.bind("SUPER + SHIFT + R", by_layout({ dwindle = hl.dsp.layout("swapsplit"), master = hl.dsp.layout("orientationprev") }))
hl.bind("SUPER + equal", by_layout({ scrolling = hl.dsp.layout("colresize +0.1"), dwindle = hl.dsp.layout("splitratio +0.1"), master = hl.dsp.layout("mfact +0.05") }))
hl.bind("SUPER + minus", by_layout({ scrolling = hl.dsp.layout("colresize -0.1"), dwindle = hl.dsp.layout("splitratio -0.1"), master = hl.dsp.layout("mfact -0.05") }))
hl.bind("SUPER + CTRL + Return", by_layout({ master = hl.dsp.layout("swapwithmaster") }))
hl.bind("SUPER + ALT + equal", by_layout({ master = hl.dsp.layout("addmaster") }))
hl.bind("SUPER + ALT + minus", by_layout({ master = hl.dsp.layout("removemaster") }))
hl.bind("SUPER + P", hl.dsp.window.pseudo())

-- === Workspaces ===
for n = 1, 9 do
	hl.bind("SUPER + " .. n, hl.dsp.focus({ workspace = n }))
	hl.bind("SUPER + SHIFT + " .. n, hl.dsp.window.move({ workspace = n }))
end
hl.gesture({ fingers = 3, direction = "horizontal", action = "workspace" })

-- === Scratchpad ===
hl.bind("SUPER + S", hl.dsp.workspace.toggle_special())
hl.bind("SUPER + SHIFT + S", hl.dsp.window.move({ workspace = "special", follow = false }))
