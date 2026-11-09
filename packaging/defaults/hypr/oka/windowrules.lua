-- Reglas de ventana. Administrado por OKARU (Ajustes → Reglas).

-- Notificaciones de apps sin robar el foco
hl.window_rule({ match = { title = "(?i)notification|toast" }, no_initial_focus = true })

-- Picture in picture
hl.window_rule({ match = { title = "^(Picture in picture|Picture-in-Picture)$" }, float = true, pin = true })

-- Ajustes y diálogos de OKARU
hl.window_rule({ match = { class = "^dev\\.okaru\\.Okaru" }, float = true, size = { 1080, 920 } })
-- Apps de OKARU
hl.window_rule({ match = { class = "^dev\\.okaru\\.Calculator$" }, float = true, center = true, size = { 440, 760 } })
hl.window_rule({ match = { class = "^dev\\.okaru\\.Viewer$" }, float = true, center = true, size = { 1000, 720 } })

-- Selector de archivos del portal y selector de pantalla para compartir
hl.window_rule({ match = { class = "^xdg-desktop-portal(-gtk)?$" }, float = true, center = true, size = { 900, 600 } })
-- Selectores que la app abre por su cuenta, sin portal (p. ej. Epic Games Launcher)
hl.window_rule({ match = { title = "^(Open File|Open Files|Open Folder|Select File|Select Folder|Save File|Save As|Abrir archivo|Abrir carpeta|Seleccionar archivo|Seleccionar carpeta|Guardar como)$" }, float = true, center = true, size = { 900, 600 } })
hl.window_rule({ match = { class = "^(hyprland-share-picker)$" }, float = true, center = true, size = { 750, 500 } })

-- Utilidades del sistema flotantes y centradas
hl.window_rule({
	match = { class = "^(pavucontrol|blueman-manager|nm-connection-editor|org\\.gnome\\.Calculator|gnome-calculator|org\\.gnome\\.FileRoller|com\\.github\\.wwmm\\.easyeffects|org\\.kde\\.kdeconnect\\.app|nwg-look|qt[56]ct)$" },
	float = true,
	center = true,
	size = { 1100, 720 },
})

-- Superficies de la shell: sin animaciones de Hyprland (la shell tiene las suyas)
hl.layer_rule({ name = "shell", match = { namespace = "^okaru-(bar-.+|notification|dock|panel|attached-panel|osd|window-switcher)$" }, no_anim = true })
