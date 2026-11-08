# Requirements

`./install.sh --check` tests all of this on your machine without changing anything.

## Minimum

| Requirement | Minimum | Why | Check |
|---|---|---|---|
| OS | Linux | Unix socket control, `/proc`, Wayland | `uname -s` |
| Session | **Wayland** | the overlay is a Wayland layer-shell surface; X11 is not supported | `echo $WAYLAND_DISPLAY` |
| Compositor | one with **wlr-layer-shell** | needed for an always-on-top, click-through, no-focus overlay | see the table below |
| GTK | 4.12 | overlay window | `pkg-config --modversion gtk4` |
| gtk4-layer-shell | 1.0 | layer surface | `pkg-config --modversion gtk4-layer-shell-0` |
| GLib | 2.74 | main loop, config parser | `pkg-config --modversion glib-2.0` |
| libmpv | client API 2.0 (mpv 0.35+) | RTSP, decoding, rendering | `pkg-config --modversion mpv` |
| json-glib | 1.6 | control protocol | `pkg-config --modversion json-glib-1.0` |
| libepoxy | 1.5 | OpenGL loading | `pkg-config --modversion epoxy` |
| Build tools | C compiler, `make`, `pkg-config` | plain Makefile build | `cc --version` |
| Installer tools | `bash` 4.4+, `python3`, `jq` | `install.sh` | |
| GPU | OpenGL (ES) 3 via EGL | video rendering | any current Mesa/NVIDIA driver |
| Network | the recorder/cameras reachable over RTSP (TCP 554 by default) | the stream | `nc -zv <recorder> 554` |

Recommended:

- **VA-API** hardware decoding (`libva-utils`, check `vainfo` lists H.264/HEVC). The 4-camera grid decodes four
  streams at once; with VA-API that costs little CPU. Without it the installer sets `hwdec = auto-safe`.
- A **view-only user** on your recorder for this program.

Arch Linux / Omarchy packages (the installer offers to install the missing ones):

```sh
sudo pacman -S --needed base-devel pkgconf glib2 json-glib libepoxy gtk4 gtk4-layer-shell mpv jq python
```

Other distributions: install the equivalent development packages (names differ); only Arch has been tested.

## Compositors

| Compositor | Status | Key bindings by the installer |
|---|---|---|
| Hyprland 0.56 (Omarchy 4.5) | **tested daily** | yes: Omarchy `bindings.lua` (`o.bind`), Lua `hyprland.lua` (`hl.bind`), `bindings.conf` (`bindd`), `hyprland.conf` (`bind`) |
| Other Hyprland versions | expected to work (layer-shell is stable); Lua config needs 0.55+ | yes |
| Sway, river, Wayfire, labwc, niri, KDE Plasma 6 | supported by gtk4-layer-shell; untested here | no: bind `cctv-overlay toggle/next/prev/audio` yourself |
| GNOME (Mutter) | **not supported** (no layer-shell) | — |
| X11 | **not supported** | — |

## Camera systems

Any recorder or camera that serves **RTSP with H.264 or H.265** works. The installer has presets for:

| System | RTSP path used | Channel meaning |
|---|---|---|
| Dahua, Amcrest, Lorex and other Dahua-based NVR/DVR | `/cam/realmonitor?channel=N&subtype=S` | recorder channel (1-based) |
| Hikvision, HiLook, Annke and other Hikvision-based NVR | `/Streaming/Channels/N01` (main) / `N02` (sub) | recorder channel |
| Reolink NVR | `/h264Preview_NN_main` / `_sub` | recorder channel |
| Uniview (UNV) NVR | `/unicast/cN/sS/live` | recorder channel |
| Separate IP cameras | per camera: Dahua/Amcrest, Hikvision, Reolink, TP-Link Tapo/VIGI, Uniview, or a custom path | one address per camera |
| Anything else | your own path template (see [configuration](configuration.md#url-templates-other-camera-systems)) | |

All cameras must share one user name and password (the usual case with a recorder). Cameras with different
credentials need a per-camera `url =` override, which puts that password in `config.ini` (discouraged).

Audio (`cctv-overlay audio`) needs the camera's audio enabled in the recorder's encoding settings; without an
audio track the speaker icon is shown crossed out.
