# CCTV Overlay for Linux

A lightweight, self-contained Linux daemon that shows a live Dahua/RTSP camera feed in a small,
click-through picture-in-picture overlay above your desktop, including over fullscreen windows on
compositors that allow it.

> **Status: working and validated with a real Dahua NVR on Hyprland; pre-release (0.1.0 not yet tagged).**
> - Verified on Hyprland 0.56.2 (Arch Linux, AMD GPU with VA-API): five real camera channels, live video over a
>   true fullscreen window, no focus change, typing reaches the fullscreen application, and every resource is
>   released when hidden. Physical mouse click-through is still to be confirmed by hand.
> - With this recorder the **main stream** opens in about 0.8 s (the sub stream took about 2.7 s because the
>   recorder starts it on demand); the stream type is configurable per camera. Nothing runs while hidden.
> - A security and a lifecycle review have been completed. Their findings are listed under
>   [Known limitations](#known-limitations) and in the changelog; the highest-priority fixes are not yet applied.
> - Other compositors and GPUs are untested. The licence is not decided yet (see below).

## What it does

- **Toggle** the overlay with one command: `cctv-overlay toggle`. You bind the key yourself.
- **Cycle** cameras with `cctv-overlay next` and `prev`, wrapping around the configured list.
- Show video in the **layer-shell `overlay` layer**, above normal windows and fullscreen content.
- **Never take keyboard focus**; the overlay is built to pass pointer input through to the window below
  (empty input region; physical click test pending).
- Decode the camera stream with VA-API hardware decoding and a low-latency libmpv profile. Sub stream is the
  default for bandwidth; the main stream can start much faster on recorders that start sub streams on demand.
- **Release everything when hidden**: no player, no RTSP connection and no GPU surface while the overlay is closed.
- Keep credentials out of config files in the repository, out of logs and out of command lines.
- Run as a user-level daemon with a local Unix-socket control interface. It has no network listener.

## Quick start

Full instructions: [`docs/install.md`](docs/install.md).

```sh
# 1. Build (Arch: sudo pacman -S --needed base-devel pkgconf glib2 json-glib libepoxy gtk4 gtk4-layer-shell mpv)
git clone <repository-url> cctv-overlay && cd cctv-overlay
make

# 2. Install without root (or see docs/install.md for a system-wide install)
make PREFIX="$HOME/.local" install

# 3. Create your config and secrets (never overwrites existing files)
mkdir -p ~/.config/cctv-overlay
cp -n ~/.local/share/doc/cctv-overlay/config.example.ini ~/.config/cctv-overlay/config.ini
( umask 077 && cp -n ~/.local/share/doc/cctv-overlay/secrets.example.ini ~/.config/cctv-overlay/secrets.ini )
$EDITOR ~/.config/cctv-overlay/config.ini ~/.config/cctv-overlay/secrets.ini   # set host, channels, password

# 4. Run (in a terminal, to see logs)
cctv-overlay daemon

# 5. In another terminal, or from a key binding you choose
cctv-overlay toggle
```

Key bindings are not chosen for you. Placeholder examples for Hyprland are in
[`docs/hyprland.md`](docs/hyprland.md) and [`integrations/hyprland/`](integrations/hyprland/).

## Commands

| Command | Effect |
|---|---|
| `cctv-overlay daemon` | Run the daemon in the foreground |
| `cctv-overlay toggle` | Show or hide the overlay |
| `cctv-overlay next` / `prev` | Next or previous camera (opens the overlay if hidden) |
| `cctv-overlay show [CAMERA]` | Open on a camera (name, id or zero-based index) |
| `cctv-overlay hide` | Close the overlay and release all resources |
| `cctv-overlay status [--json]` | State, camera, last error (never credentials or URLs) |
| `cctv-overlay list` | List configured cameras (`*` marks the current one) |
| `cctv-overlay reload` | Re-read `config.ini` and `secrets.ini` |
| `cctv-overlay quit` | Stop the daemon |
| `cctv-overlay ping` | Health check |

Exit codes and details: [`docs/behaviour.md`](docs/behaviour.md).

## Known limitations

- **Physical click-through is unverified.** The overlay sets an empty input region and a pointer-event probe
  confirmed the compositor sends it no pointer events; one real click still needs checking by hand.
- **Rapid camera commands are not coalesced** (review finding F3). A few commands per second are fine; hundreds
  per second can block the UI while players are torn down and rebuilt.
- **Error classification is heuristic** (F7): a warning containing a number such as `401` can pick the wrong
  friendly message. The message is cosmetic; the stream state is correct.
- **Developer option channel**: `CCTV_OVERLAY_MPV_OPTS` can set arbitrary libmpv options. It is for the same
  user who can already run code as that user, and is documented as unsafe for untrusted environments.
- **A per-camera `url =` override may contain credentials.** They are registered with the redactor and a warning
  is logged, but the string stays in the configuration object. Prefer `secrets.ini`.
- **Plain RTSP is unencrypted.** A hostile on-path device could downgrade authentication. Use a dedicated
  low-privilege account and a trusted network; `rtsps://` is not implemented.
- **Direct scanout is disabled** for a fullscreen window on the same monitor while the overlay is mapped
  (Hyprland behaviour). Scanout was already off on the development machine; the frame-rate cost in a game has
  not been measured.
- Only Hyprland has been tested. GNOME and X11 are not supported; other Wayland compositors are untested.
- The systemd unit's sandboxing has not been exercised in a live user session.
- `start_hidden`, `preview_snapshot`, `preload_next` and `opacity` are accepted but not implemented.

## Documentation

| Document | For |
|---|---|
| [`docs/install.md`](docs/install.md) | Dependencies, build, install, systemd user unit |
| [`docs/configuration.md`](docs/configuration.md) | Every config key, secrets file, example |
| [`docs/hyprland.md`](docs/hyprland.md) | Binding commands in Hyprland, layer behaviour |
| [`docs/troubleshooting.md`](docs/troubleshooting.md) | Common errors and how to read the logs |
| [`docs/behaviour.md`](docs/behaviour.md) | Commands, states, exit codes, security model |
| [`docs/overlay.md`](docs/overlay.md) | Overlay window, layers, monitors, video engine |
| [`ARCHITECTURE.md`](ARCHITECTURE.md) | Components, state machine, design rationale |
| [`docs/adr/`](docs/adr/) | Architecture decision records |
| [`CHANGELOG.md`](CHANGELOG.md) | Release notes |

## Architecture

One daemon owns one GTK 4 layer-shell window and one libmpv player. The window uses an empty input region
and keyboard mode `none`, so it is click-through and never takes focus. Video renders through libmpv's OpenGL
render API into a `GtkGLArea`, decoded with VA-API. A thin CLI sends one JSON line per request over a Unix
socket in `$XDG_RUNTIME_DIR`. Details and trade-offs are in [`ARCHITECTURE.md`](ARCHITECTURE.md).

## Limitations

- **Wayland with layer-shell only.** The daemon refuses to start with `overlay unavailable` on compositors
  without wlr layer-shell (GNOME, X11). Per the gtk4-layer-shell documentation, wlroots-based compositors and
  KDE Wayland are supported in principle; only Hyprland 0.56.2 has been tested.
- **Layer `overlay` only by default.** On Hyprland 0.56 a `top` layer is hidden under fullscreen windows.
- **Scanout cost.** While the overlay is visible, direct scanout of a fullscreen window on that monitor is off.
  The effect on game performance has not been measured yet.
- **Real camera not yet tested.** Use the loopback test harness (`make check-real`) to check your setup first.
- **Recorder support is Dahua-style RTSP only.** The URL builder uses the Dahua path and channel scheme. Other
  cameras need a `url` override, which puts the credentials in the config file (discouraged).
- **Some settings are accepted but not used yet:** `start_hidden`, `preview_snapshot`, `preload_next` and
  `opacity`. See [`docs/configuration.md`](docs/configuration.md).
- **No audio.** Video only, by design.
- **No recording, snapshots to disk, motion alerts or PTZ.** Out of scope for 0.1.
- **Password in memory.** The daemon keeps the recorder password in process memory while it runs, so a core
  dump could contain it.

## Security summary

- The repository contains no credentials. The example config has no password.
- The password lives in `secrets.ini` (mode `0600`) or in the `CCTV_OVERLAY_PASSWORD` environment variable,
  never on a command line and never in `config.ini`.
- Logs and error messages are redacted. The RTSP URL with credentials is built only when a stream starts and
  is wiped after use.
- The control socket is in `$XDG_RUNTIME_DIR/cctv-overlay/` (directory `0700`, socket `0600`). Only the same user
  is accepted. There is no network listener.
- Installation never overwrites an existing `config.ini` or `secrets.ini`; it installs examples only.
- Nothing edits your compositor or desktop configuration automatically.

A formal security review has not been completed yet.

## Licence

**To be decided.** The project is not yet released under any licence. MIT is the proposed default. Until a
`LICENSE` file is added, the code is not licensed for reuse by others.
