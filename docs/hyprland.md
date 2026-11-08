# Hyprland integration

cctv-overlay does not bind keys and does not edit your compositor configuration. You bind the commands
yourself. Two optional snippets show the syntax:

| File | For |
|---|---|
| [`integrations/hyprland/example.lua`](../integrations/hyprland/example.lua) | Hyprland 0.55 and later, Lua configuration (`hyprland.lua`), including Omarchy's Lua config style |
| [`integrations/hyprland/example.conf`](../integrations/hyprland/example.conf) | Classic hyprlang (`hyprland.conf`), Hyprland 0.54 and earlier. The hyprlang format is deprecated since 0.55 |

**The keys in both snippets are placeholders** (`SUPER + F9` and so on). Pick your own keys. Copy the lines you
want into your own file by hand. Nothing is applied automatically, and the snippets are not `source`d or
included by anything in this project.

## The commands to bind

| Action | Command |
|---|---|
| Show or hide the overlay | `cctv-overlay toggle` |
| Next camera | `cctv-overlay next` |
| Previous camera | `cctv-overlay prev` |
| Open on a specific camera | `cctv-overlay show "Front Door"` (name, id, or zero-based index) |
| Close the overlay | `cctv-overlay hide` |

`next` and `prev` also open the overlay when it is hidden. `hide` always closes it and releases the
stream and the GPU decoder.

Make sure `cctv-overlay` is on the `PATH` of the session that runs your compositor. Compositor key bindings
run commands through the shell environment that Hyprland was started with.

## Lua (Hyprland 0.55+)

```lua
local mainMod = "SUPER"  -- example modifier; change to suit

hl.bind(mainMod .. " + F9", hl.dsp.exec_cmd("cctv-overlay toggle"))
hl.bind(mainMod .. " + F10", hl.dsp.exec_cmd("cctv-overlay next"))
hl.bind(mainMod .. " + SHIFT + F10", hl.dsp.exec_cmd("cctv-overlay prev"))
```

`hl.dsp.exec_cmd` runs the command asynchronously, so the compositor does not wait for it. This is the form
the Hyprland wiki documents for binds. The example has not been run on this machine; see the header of the file.

## Classic hyprlang (0.54 and earlier)

```conf
# Example modifier; change to suit.
$mainMod = SUPER

bind = $mainMod, F9, exec, cctv-overlay toggle
bind = $mainMod, F10, exec, cctv-overlay next
bind = $mainMod SHIFT, F10, exec, cctv-overlay prev
```

## Layer behaviour

The overlay is a layer-shell surface in the `overlay` layer, namespace `cctv-overlay`, with keyboard mode
`none` and an empty input region. The project's tests used no window rules or layer rules. Check it with:

```sh
hyprctl layers | grep -A3 cctv-overlay
```

The entry appears under the monitor it is shown on, at level 3 (the overlay level). It is not listed by
`hyprctl clients`, because it is not a client window.

What was verified on Hyprland 0.56.2 (see [`overlay.md`](overlay.md) and the ADRs):

- The overlay is visible above a true fullscreen window on the chosen monitor (the `overlay` layer).
- Focus does not move to the overlay, and typing still reaches the fullscreen application.
- The empty input region delivered no pointer events to the overlay in a pointer-event probe. A physical
  mouse-click test on a button under the overlay is still pending.

Known effects:

- A `top` layer is hidden under fullscreen windows on 0.56.2, so keep `layer = overlay` (the default).
- While the overlay is visible on a monitor, direct scanout of a fullscreen window on that monitor is off. The
  performance cost in games has not been measured yet.
- With `follow_mouse` enabled, moving the pointer across the overlay can change focus to the window
  underneath, as with any window. The overlay itself never takes focus.

## Choosing the monitor

Set `[general] monitor` in `config.ini`:

- Use an explicit connector name (`DP-1`, from `hyprctl monitors`). This is the reliable choice.
- `focused` lets the compositor decide when the overlay is shown. It depends on where the pointer and focus
  are at that moment.

## Reloading

After editing your Hyprland config, reload it the way you normally do (for example `hyprctl reload`). Config
errors are reported by Hyprland; check its notifications or log if a bind does not work.

## Notes

- If the bind starts the daemon (because it is not running), the daemon starts without a terminal and its
  logs go nowhere. For logs, run `cctv-overlay daemon` in a terminal or use the systemd user unit
  ([install guide](install.md#optional-systemd-user-unit)).
- The overlay is also toggled and cycled from the CLI, so any other launcher, script or bar can use the same
  commands.
