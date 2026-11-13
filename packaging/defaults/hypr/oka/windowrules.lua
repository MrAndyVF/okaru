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
hl.window_rule({ match = { class = "^dev\\.okaru\\.Player$" }, float = true, center = true, size = { 1100, 700 } })
hl.window_rule({ match = { class = "^dev\\.okaru\\.Editor$" }, float = true, center = true, size = { 1100, 760 } })
hl.window_rule({ match = { class = "^dev\\.okaru\\.Files$" }, float = true, center = true, size = { 1300, 800 } })
hl.window_rule({ match = { class = "^dev\\.okaru\\.Pdf$" }, float = true, center = true, size = { 760, 960 }, opacity = 1 })

-- Nautilus y LocalSend
hl.window_rule({ match = { class = "^org\\.gnome\\.Nautilus$" }, float = true, center = true, size = { 1300, 800 } })
hl.window_rule({ match = { class = "^org\\.localsend\\.localsend_app$" }, float = true, center = true, size = { 600, 800 } })

-- Selector de archivos del portal y selector de pantalla para compartir
hl.window_rule({ match = { class = "^xdg-desktop-portal(-gtk)?$" }, float = true, center = true, size = { 900, 600 } })
-- Selectores que la app abre por su cuenta, sin portal (p. ej. Epic Games Launcher)
hl.window_rule({ match = { title = "^(Open File|Open Files|Open Folder|Select File|Select Folder|Save File|Save As|Abrir archivo|Abrir carpeta|Seleccionar archivo|Seleccionar carpeta|Guardar como)$" }, float = true, center = true, size = { 900, 600 } })
hl.window_rule({ match = { class = "^(hyprland-share-picker)$" }, float = true, center = true, size = { 750, 500 } })

-- Steam: los popups flotan, la ventana principal va en mosaico a todo lo ancho
hl.window_rule({ match = { class = "^steam$" }, float = true })
hl.window_rule({ match = { class = "^steam$", title = "^Steam$" }, tile = true, scrolling_width = 1 })
hl.window_rule({ match = { class = "^steam$", title = "^Friends List" }, size = { 400, 700 } })
-- Chats y diálogos: Steam recuerda tamaños de pantalla completa. Solo con título: los menús desplegables
-- (título vacío) deben quedar donde Steam los pone
hl.window_rule({ match = { class = "^steam$", title = "^.+$", float = true }, max_size = { 1000, 850 }, center = true })
hl.window_rule({ match = { class = "^steam$", title = "^notificationtoasts" }, pin = true, no_initial_focus = true })
-- Juegos de Steam en pantalla completa
hl.window_rule({ match = { class = "^steam_app_" }, fullscreen = true })

-- Launchers de juegos en mosaico a todo lo ancho (Heroic, Faugus, Prism y Epic Games Launcher nativo)
hl.window_rule({ match = { class = "^(heroic|io\\.github\\.Faugus\\.faugus-launcher|org\\.prismlauncher\\.PrismLauncher|EpicGamesLauncher)$" }, tile = true, scrolling_width = 1 })

-- Utilidades del sistema flotantes y centradas
hl.window_rule({
	match = { class = "^(pavucontrol|blueman-manager|nm-connection-editor|org\\.gnome\\.FileRoller|org\\.gnome\\.DiskUtility|com\\.github\\.wwmm\\.easyeffects|org\\.kde\\.kdeconnect\\.app|nwg-look|qt[56]ct|goverlay|org\\.cachyos\\.(KernelManager|scx-manager|hello|cachyos-pi)|nvidia-settings|btrfs-assistant|io\\.missioncenter\\.MissionCenter)$" },
	float = true,
	center = true,
	size = { 1100, 720 },
})

-- Superficies de la shell: sin animaciones de Hyprland (la shell tiene las suyas)
hl.layer_rule({ name = "shell", match = { namespace = "^okaru-(bar-.+|notification|dock|panel|attached-panel|osd|window-switcher)$" }, no_anim = true })
