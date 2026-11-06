-- Distribución y decoración de ventanas. Administrado por OKARU (Ajustes → Ventanas).
hl.config({
	general = {
		gaps_in = 4,
		gaps_out = 8,
		border_size = 2,
		layout = "dwindle",
	},
	decoration = {
		rounding = 0, -- esquinas rectas, como la shell
		blur = { size = 5, passes = 2, popups = true },
		shadow = { enabled = true, range = 16, render_power = 3, color = "rgba(00000040)" },
	},
	misc = {
		disable_hyprland_logo = true,
		disable_splash_rendering = true,
	},
	ecosystem = {
		no_update_news = true,
		no_donation_nag = true,
	},
})
