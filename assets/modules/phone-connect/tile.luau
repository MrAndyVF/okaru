-- tile.luau
-- Phone Connect - control-center tile (thin UI entry).
--
-- Shows a compact status: connected count or selected device battery. Click
-- opens the details panel. Reads state from the service entry; no DBus here.

-- Pick which panel id to toggle based on the user's panel_placement setting.

-- Local translation: reads from pc.trTable (published by service) so users
-- can switch language at runtime, unlike okaru.tr() which follows system locale.
local function t(key)
	local table = okaru.state.get("pc.trTable") or {}
	return table[key] or key
end

local function panelId()
	local mode = okaru.getConfig("panel_placement")
	if mode == "floating" then
		return "okaru/phone-connect:details_floating"
	elseif mode == "widget" then
		return "okaru/phone-connect:details_widget"
	end
	return "okaru/phone-connect:details"
end

local function selectedDevice()
	local devices = okaru.state.get("pc.devices") or {}
	local order = okaru.state.get("pc.order") or {}
	local sel = okaru.state.get("pc.selected")
	local id = sel
	if not id or not devices[id] then id = order[1] end
	if id then return devices[id] end
	return nil
end

local function countReachable()
	local devices = okaru.state.get("pc.devices") or {}
	local order = okaru.state.get("pc.order") or {}
	local n = 0
	for _, id in ipairs(order) do
		local d = devices[id]
		-- connected = paired AND reachable; reachable alone is mere visibility
		if d and d.isPaired and d.isReachable == true then n = n + 1 end
	end
	return n
end

local function render()
	local backend = okaru.state.get("pc.backend") or { available = false }
	if not backend.available then
		shortcut.setIcon("phone-off")
		shortcut.setLabel(t("status.no_backend"))
		shortcut.setActive(false)
		shortcut.setEnabled(false)
		return
	end

	local n = countReachable()
	local d = selectedDevice()
	shortcut.setEnabled(true)

	-- connected = paired AND reachable; reachable alone is mere visibility
	if d and d.isPaired and d.isReachable and type(d.batteryCharge) == "number" and d.batteryCharge >= 0 then
		-- show selected device battery when reachable
		shortcut.setIcon("device-mobile")
		shortcut.setLabel(tostring(d.batteryCharge) .. "%")
		shortcut.setActive(true)
	else
		shortcut.setIcon("device-mobile")
		shortcut.setLabel(n > 0 and (tostring(n) .. " " .. t("status.connected"))
			or t("status.no_devices"))
		shortcut.setActive(n > 0)
	end
end

function update()
	okaru.setUpdateInterval(3000)
	render()
end

function onClick()
	okaru.togglePanel(panelId())
end

okaru.state.watch("pc.devices", render)
okaru.state.watch("pc.order", render)
okaru.state.watch("pc.selected", render)
okaru.state.watch("pc.backend", render)
okaru.state.watch("pc.trTable", render)

render()
