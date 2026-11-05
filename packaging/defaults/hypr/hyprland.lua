-- Hyprland (Lua) — configuración de fábrica de OKARU. https://wiki.hypr.land/Configuring/Start/
-- La shell administra hypr/oka/*.lua desde Ajustes (Pantallas, Ventanas, Entrada, Atajos, Reglas).
-- Tus atajos propios van en oka/user-binds.lua: OKARU nunca lo reescribe.
-- La shell la arranca su servicio de systemd --user (okaru.service), no hl.exec_cmd.

require("oka.outputs")
require("oka.layout")
require("oka.cursor")
require("oka.inputs")
require("oka.windowrules")
require("oka.binds")

-- Colores de la shell (hypr/okaru.lua, lo genera la plantilla "hyprland"); pcall porque no existe hasta el
-- primer arranque de la shell.
if pcall(require, "okaru") then require("okaru").apply_theme() end

require("oka.user-binds")
