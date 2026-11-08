-- cctv-overlay: EXAMPLE key bindings for Hyprland's Lua configuration (Hyprland 0.55 and later).
--
-- STATUS: UNTESTED EXAMPLE. The syntax follows the Hyprland wiki (Binds page: hl.bind(keys, dispatcher)
-- and hl.dsp.exec_cmd(cmd)). It has not been loaded into a running Hyprland by this project.
--
-- Nothing here is applied automatically. Copy the lines you want into YOUR OWN hyprland.lua by hand,
-- and choose your own keys. The keys below are placeholders only.
--
-- The commands:
--   cctv-overlay toggle   show or hide the overlay
--   cctv-overlay next     next camera (opens the overlay if hidden)
--   cctv-overlay prev     previous camera (opens the overlay if hidden)

local mainMod = "SUPER" -- example modifier; change to suit

hl.bind(mainMod .. " + F9", hl.dsp.exec_cmd("cctv-overlay toggle"))
hl.bind(mainMod .. " + F10", hl.dsp.exec_cmd("cctv-overlay next"))
hl.bind(mainMod .. " + SHIFT + F10", hl.dsp.exec_cmd("cctv-overlay prev"))
