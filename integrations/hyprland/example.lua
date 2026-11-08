-- cctv-overlay: EXAMPLE key bindings for Hyprland's Lua configuration (Hyprland 0.55 and later).
--
-- STATUS: the hl.bind(keys, hl.dsp.exec_cmd(cmd)) form is the same call Omarchy's o.bind() helper makes
-- internally, and an equivalent o.bind() pair is in daily use on Hyprland 0.56.2 / Omarchy 4.5. The
-- placeholder keys below are examples only.
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

-- On Omarchy, the helper form (put it in ~/.config/hypr/bindings.lua, which Omarchy loads after its defaults).
-- Use an absolute path if cctv-overlay is not on the PATH that Hyprland launches commands with:
--
--   o.bind("CTRL + KP_Multiply", "Toggle CCTV overlay", "/home/YOU/.local/bin/cctv-overlay toggle")
--   o.bind("KP_Multiply", "Next CCTV camera", "/home/YOU/.local/bin/cctv-overlay next")
--
-- Notes: KP_Multiply is the numpad asterisk and works with NumLock on or off. Binding it without a modifier
-- takes that key away from every application while Hyprland runs.
