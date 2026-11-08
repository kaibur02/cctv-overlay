# Configuration

cctv-overlay uses two INI files in one directory, `~/.config/cctv-overlay/` (or `$XDG_CONFIG_HOME/cctv-overlay/`):

| File | Holds | Permissions |
|---|---|---|
| `config.ini` | Layout, stream and recorder settings, camera list. **No secrets.** | normal (`0644` or `0600`) |
| `secrets.ini` | Recorder password, optionally username | **`0600`** |

The easiest way to create both is `./install.sh` (it asks about your camera system, cameras, monitor and keys,
and reads the password with a hidden prompt). Templates are also in `examples/` in the source tree and under
`/usr/share/doc/cctv-overlay/` after a package install. The program itself never writes these files.
`cctv-overlay check` validates both files offline and never prints the password.

Syntax rules (GKeyFile):

- Comments are **full lines** starting with `#`. Do **not** put a comment after a value on the same line:
  `width = 480  # pixels` makes the value `480  # pixels` and it is rejected.
- Section names are case-sensitive. Use `[camera.<id>]` for each camera; the order of these sections is the
  cycling order.
- Values are literal. Do not quote them and do not use escape sequences.
- Unknown sections or keys are errors, so typos are caught. Errors name the file, section and key.

Apply changes with `cctv-overlay reload`. An invalid file is rejected and the previous configuration keeps running.

## `config.ini`

### `[general]`

| Key | Default | Values | Notes |
|---|---|---|---|
| `log_level` | `info` | `debug`, `info`, `warning`, `error` | `debug` is verbose. Logs are redacted but still review them before sharing (see [troubleshooting](troubleshooting.md#sharing-logs)). |
| `monitor` | `focused` | `focused`, `primary`, or a connector name such as `DP-1` | Where the overlay appears. A connector name is the reliable choice; see below. |
| `start_hidden` | `true` | boolean | Accepted but has no effect yet: the daemon always starts hidden. |

`monitor` values:

- `DP-1` (any connector name from `hyprctl monitors`): the overlay always appears on that output. If the
  connector does not exist, a warning is logged and the compositor chooses.
- `focused`: GTK has no portable focused-monitor API, so the compositor chooses at show time. On Hyprland it
  is the focused monitor at that moment. Other compositors may differ.
- `primary`: the first monitor GDK lists. GTK 4 has no real "primary" concept.

### `[overlay]`

| Key | Default | Values | Notes |
|---|---|---|---|
| `width` | `480` | 64–7680 | Pixels, logical size. |
| `height` | `270` | 64–4320 | Pixels, logical size. |
| `anchor` | `top-right` | `top-left`, `top-right`, `bottom-left`, `bottom-right` | Screen corner. |
| `margin_x` | `24` | 0–10000 | Distance from the left or right edge, whichever is anchored. |
| `margin_y` | `24` | 0–10000 | Distance from the top or bottom edge, whichever is anchored. |
| `layer` | `overlay` | `overlay`, `top` | Keep `overlay`. See [note on layers](#layer-overlay-or-top). |
| `show_label` | `true` | boolean | Camera name chip at the top left. |
| `show_status` | `true` | boolean | Status line shown while not live. |
| `opacity` | `1.0` | number | Accepted but **not applied** yet. |

#### Layer: `overlay` or `top`

Use `overlay`, the default. On Hyprland 0.56 a `top` layer fades out under fullscreen windows (decision
D-018 in the project records), so the overlay would disappear over fullscreen content. `top` exists for
compositors where that behaviour is wanted. A visible overlay also stops direct scanout of a fullscreen
window on the same monitor while it is shown (see [limitations](../README.md#limitations)).

### `[stream]`

| Key | Default | Values | Notes |
|---|---|---|---|
| `transport` | `tcp` | `tcp`, `udp` | RTSP transport. TCP is the stable default. UDP may lower latency but can drop packets on a busy LAN. |
| `subtype` | `1` | `0`–`3` | Default Dahua stream: `0` = main, `1` = sub stream. Overridable per camera. |
| `hwdec` | `vaapi` | `vaapi`, `vaapi-copy`, `auto-safe`, `no` | Hardware decode. `vaapi` falls back to software when the hardware path is unusable. |
| `reconnect` | `true` | boolean | Retry with backoff after an error or stall. |
| `audio` | `false` | boolean | Camera audio when the daemon starts. Toggled at runtime with `cctv-overlay audio` (or `audio on`/`off`); a speaker icon shows while it is on. Only the single-camera view plays audio. The camera's audio must be enabled in the recorder's encoding settings. |
| `preview_snapshot` | `true` | boolean | Accepted but has no effect yet (no snapshot preview in this build). |
| `preload_next` | `false` | boolean | Accepted but has no effect yet (no preloading). |

### `[recorder]`

| Key | Default | Notes |
|---|---|---|
| `host` | *(none, required)* | Recorder address, for example `192.168.1.100`. Required unless every camera has a `url`. |
| `port` | `554` | RTSP port, 1–65535. |
| `username` | *(empty)* | Recorder user. Prefer `secrets.ini` or `CCTV_OVERLAY_USERNAME`. |
| `path` | `/cam/realmonitor` | RTSP path. Without `{placeholders}` (the default) the Dahua query `?channel=N&subtype=S` is appended. With placeholders it is a template, see below. |

The **password is not a key here.** It is rejected in `config.ini`. Put it in `secrets.ini` (see below).

#### URL templates (other camera systems)

The stream URL is `rtsp://USER:PASSWORD@host:port` + the expanded `path`. Credentials are always inserted by the
program from `secrets.ini`; they never appear in `config.ini`. Placeholders:

| Placeholder | Value |
|---|---|
| `{channel}` | camera channel (`3`) |
| `{channel02}` | channel with two digits (`03`) |
| `{subtype}` | `0` main stream, `1` sub stream |
| `{stream}` | `1` main stream, `2` sub stream |
| `{mainsub}` | `main` or `sub` |

| System | `path` |
|---|---|
| Dahua, Amcrest, Lorex | `/cam/realmonitor` (default) |
| Hikvision, HiLook, Annke NVR | `/Streaming/Channels/{channel}0{stream}` |
| Reolink NVR | `/h264Preview_{channel02}_{mainsub}` |
| Uniview NVR | `/unicast/c{channel}/s{subtype}/live` |

Unknown placeholders are configuration errors.

### `[camera.<id>]`

One section per camera. `<id>` is a short identifier you choose (letters, digits, `_`), used by
`show <id>`.

| Key | Required | Notes |
|---|---|---|
| `name` | yes | Display name, unique (case-insensitive). Used by `show <name>`. |
| `channel` | yes, unless `url` is set | Dahua channel number, starting at 1. Check it on your recorder; do not assume channels exist. |
| `subtype` | no | Overrides `[stream] subtype` for this camera. |
| `host` | no | This camera's own address (separate IP cameras without a recorder). Overrides `[recorder] host`. |
| `port` | no | This camera's RTSP port. Overrides `[recorder] port`. |
| `path` | no | This camera's RTSP path, used **literally** (no Dahua query is appended); placeholders allowed. With `path` set, `channel` is optional. Examples: Hikvision camera `/Streaming/Channels/10{stream}`, Reolink `/h264Preview_01_{mainsub}`, TP-Link Tapo `/stream{stream}`, Dahua/Amcrest camera `/cam/realmonitor?channel=1&subtype={subtype}`. |
| `url` | no | Full `rtsp://` URL. Bypasses the recorder builder. **Discouraged**: the URL would contain the password. Use only for non-secret sources. |

Example (the `url` form is shown commented out):

```ini
[camera.front]
name = Front Door
channel = 1

[camera.driveway]
name = Driveway
channel = 2
subtype = 0
```

Separate IP cameras (no recorder), sharing one user name and password:

```ini
[recorder]
username = viewer

[camera.porch]
name = Porch
host = 192.168.1.21
path = /Streaming/Channels/10{stream}
```

### `[grid]`

The grid (overview) view shows up to four cameras in a 2×2 layout. When it is available it is the view
`toggle` always opens on, and `next`/`prev` cycle **grid → camera 1 → … → camera N → grid** (both ways,
wrapping). Each tile is its own stream with its own reconnect logic, so one offline camera does not affect the
others; the overlay state is `live` while any tile is live.

| Key | Default | Notes |
|---|---|---|
| `enabled` | `true` | `false` restores the plain camera ring (toggle opens the current camera). The grid also needs at least 2 cameras. |
| `cameras` | first four | Comma-separated camera ids or names, 2–4, in tile order (top-left, top-right, bottom-left, bottom-right). |
| `subtype` | per camera | Stream for the grid tiles, e.g. `1` to use the lighter sub streams for the overview only. |

The grid never plays audio. Four streams decode at once, so VA-API is strongly recommended; with the main stream
expect roughly four times the bandwidth of one camera.

## `secrets.ini`

```ini
# Recorder credentials. Mode 0600. Never commit this file.
[recorder]
password = your-recorder-password
# username = viewer
```

- Values are literal: no quotes, no escapes. A `#` inside a password is part of the password, but a comment
  must still be on its own line.
- The file must be mode `0600`. If group or other can read it, the daemon logs a warning that names the file and its mode.
- Environment variables override the file: `CCTV_OVERLAY_PASSWORD` and `CCTV_OVERLAY_USERNAME`. For a systemd user
  service, prefer the file (environment values are visible in some process listings and in unit files).
- Never pass the password on a command line (it would appear in `ps`). cctv-overlay never accepts it that way.

Create it without a race window on the mode:

```sh
mkdir -p ~/.config/cctv-overlay
( umask 077 && cp -n /usr/share/doc/cctv-overlay/secrets.example.ini ~/.config/cctv-overlay/secrets.ini )
${EDITOR:-nano} ~/.config/cctv-overlay/secrets.ini   # replace the placeholder password
```

## Complete minimal example

```ini
[general]
monitor = DP-1

[overlay]
width = 480
height = 270
anchor = top-right

[recorder]
host = 192.168.1.100
port = 554
username = viewer

[camera.front]
name = Front Door
channel = 1
```

Pair it with `secrets.ini` containing `password` (mode `0600`).

## Start-up latency: main stream vs sub stream

How quickly a camera appears is dominated by the recorder, not by this program. Measured on one Dahua
recorder (HEVC, wired LAN), toggle to first picture:

| `subtype` | Time to picture | Cost while visible |
|---|---|---|
| 1 (sub stream) | about 2.7 s (about 1.6 s for the recorder to answer the RTSP `DESCRIBE`, then about 2 s to the first keyframe) | about 0.7% of one core, 0.6 Mbit/s |
| 0 (main stream) | about 0.8 s | about 2% of one core, 3 Mbit/s |

Client-side tuning (UDP, video-only demuxing, probe settings) made no measurable difference in the same test. If the
overlay is slow to appear, try `subtype = 0` (globally in `[stream]`, or per camera). Nothing runs while the overlay is
hidden, so the extra cost applies only while it is visible. Your recorder may differ; measure with
`CCTV_OVERLAY_MPV_TRACE` (see `docs/troubleshooting.md`).
