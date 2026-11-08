# Hyprland integration

The program itself never binds keys. `./install.sh` asks which keys you want and writes them into a marked
block in your Hyprland config (after a timestamped backup), in the format your setup uses:

| Your config | Installer writes |
|---|---|
| Omarchy `~/.config/hypr/bindings.lua` | `o.bind("CTRL + KP_Multiply", "Toggle CCTV overlay", "…/cctv-overlay toggle")` |
| Lua `~/.config/hypr/hyprland.lua` (0.55+) | `hl.bind("CTRL + KP_Multiply", hl.dsp.exec_cmd("…/cctv-overlay toggle"))` |
| Omarchy (older) `~/.config/hypr/bindings.conf` | `bindd = CTRL, KP_Multiply, Toggle CCTV overlay, exec, …/cctv-overlay toggle` |
| `~/.config/hypr/hyprland.conf` | `bind = CTRL, KP_Multiply, exec, …/cctv-overlay toggle` |

The block starts with `>>> cctv-overlay keybindings >>>` and ends with `<<< cctv-overlay keybindings <<<`.
Re-running the installer replaces it; `./install.sh --uninstall` removes it. If Hyprland reports a new config
error after the change, the installer restores the backup by itself.

To bind by hand instead, two optional snippets show the syntax:

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
| Show or hide the overlay (opens on the grid) | `cctv-overlay toggle` |
| Next view (grid → cameras → grid) | `cctv-overlay next` |
| Previous view | `cctv-overlay prev` |
| Camera audio on/off | `cctv-overlay audio` |
| Grid view | `cctv-overlay grid` |
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

## Omarchy (tested form)

On Omarchy, add bindings to `~/.config/hypr/bindings.lua` (your user-level file, loaded after Omarchy's defaults;
never edit Omarchy's own files). This pair is in daily use on Hyprland 0.56.2 / Omarchy 4.5:

```lua
o.bind("CTRL + KP_Multiply", "Toggle CCTV overlay", "/home/YOU/.local/bin/cctv-overlay toggle")
o.bind("KP_Multiply", "Next CCTV view", "/home/YOU/.local/bin/cctv-overlay next")
o.bind("KP_Divide", "Previous CCTV view", "/home/YOU/.local/bin/cctv-overlay prev")
o.bind("CTRL + KP_Subtract", "Toggle CCTV audio", "/home/YOU/.local/bin/cctv-overlay audio")
```

- `KP_Multiply`, `KP_Divide` and `KP_Subtract` are the numpad `*`, `/` and `-` (they work with NumLock on or
  off); the keys on the main row are different keys.
- A binding without a modifier takes that key away from all applications while Hyprland runs.
- Use an absolute path: Hyprland may not have `~/.local/bin` on the `PATH` it launches commands with.
- The daemon starts automatically on the first command, and an auto-started daemon logs (redacted) to
  `$XDG_STATE_HOME/cctv-overlay/daemon.log` (default `~/.local/state/cctv-overlay/daemon.log`, mode 0600).
- After editing, Hyprland reloads on save; check `hyprctl configerrors` and `hyprctl binds`.
