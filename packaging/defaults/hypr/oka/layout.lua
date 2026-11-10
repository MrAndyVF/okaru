-- Distribución y decoración de ventanas. Administrado por OKARU (Ajustes → Ventanas).
hl.config({
	general = {
		gaps_in = 4,
		gaps_out = 8,
		border_size = 2,
		resize_on_border = false,
		allow_tearing = true,
		layout = "scrolling",
	},
	-- Layout scrolling tipo niri
	scrolling = {
		direction = "right",
		column_width = 0.5,
		explicit_column_widths = "0.5, 0.66667, 1.0",
		focus_fit_method = 1,
		fullscreen_on_one_column = true,
		follow_focus = true,
	},
	decoration = {
		rounding = 0, -- esquinas rectas, como la shell
		active_opacity = 0.95,
		inactive_opacity = 0.95,
		dim_inactive = false,
		blur = {
			size = 5,
			passes = 3,
			contrast = 0.9,
			popups = true, -- blur en menús contextuales
			popups_ignorealpha = 0.2,
		},
		shadow = {
			enabled = true,
			range = 20,
			render_power = 3,
			offset = "0 2",
			color = "rgba(00000040)",
		},
	},
	group = {
		groupbar = { blur = true },
	},
	misc = {
		animate_manual_resizes = false,
		animate_mouse_windowdragging = false,
		disable_hyprland_logo = true,
		disable_splash_rendering = true,
	},
	ecosystem = {
		no_update_news = true,
		no_donation_nag = true,
	},
})

-- Animaciones: resortes críticamente amortiguados como niri (damping = 2·√stiffness con mass 1)
hl.curve("niri", { type = "spring", stiffness = 800, damping = 56.57, mass = 1 })
hl.curve("niri_ws", { type = "spring", stiffness = 1000, damping = 63.25, mass = 1 })
hl.animation({ leaf = "windowsIn", enabled = true, speed = 3, bezier = "default" })
-- Cierre de ventanas más lento y suave (speed en décimas de segundo)
hl.animation({ leaf = "windowsOut", enabled = true, speed = 6, bezier = "default" })
hl.animation({ leaf = "fadeOut", enabled = true, speed = 6, bezier = "default" })
-- Cambio de workspace con deslizamiento vertical, como niri
hl.animation({ leaf = "workspaces", enabled = true, speed = 5, spring = "niri_ws", style = "slidevert" })
hl.animation({ leaf = "windowsMove", enabled = true, speed = 4, spring = "niri" })
hl.animation({ leaf = "fade", enabled = true, speed = 3, bezier = "default" })
hl.animation({ leaf = "border", enabled = true, speed = 3, bezier = "default" })
