# Installation

cctv-overlay is a C program built with `make`. It needs a Wayland session with a compositor that supports
the wlr layer-shell protocol (see [limitations](../README.md#limitations)). It was built and tested on Hyprland
0.56.2 under Arch Linux.

Installation never touches your configuration. It writes the binary and example files only. You create
your own `config.ini` and `secrets.ini` (see [configuration](configuration.md)).

## 1. Dependencies (Arch Linux)

Build and run dependencies:

```sh
sudo pacman -S --needed base-devel pkgconf glib2 json-glib libepoxy gtk4 gtk4-layer-shell mpv
```

| Package | Why |
|---|---|
| `base-devel`, `pkgconf` | Compiler, `make`, `pkg-config` (the build is a plain Makefile; no cmake or meson) |
| `glib2`, `json-glib` | Config parsing (`GKeyFile`), IPC JSON |
| `gtk4` | Overlay window |
| `gtk4-layer-shell` | Layer-shell surface. **Must be linked first** (the Makefile already does this) |
| `mpv` | Provides `libmpv` (render API, VA-API decode) |
| `libepoxy` | OpenGL function loading for the video surface |

Optional, only for the opt-in integration tests (`make check`, `make check-real`):
`jq`, `ffmpeg`, `grim`, `python3`, `iproute2` (`ss`), and `hyprctl` on Hyprland.

GPU: hardware decode uses VA-API. Check with `vainfo` that your GPU supports H.264 or HEVC decode. If it does
not, set `hwdec = no` (software decode, more CPU).

Other distributions: the same libraries are needed (GTK 4, gtk4-layer-shell, libmpv, epoxy, GLib, json-glib).
Package names differ; the project has been tested on Arch only.

## 2. Build

```sh
git clone <repository-url> cctv-overlay
cd cctv-overlay
make
./cctv-overlay --version
```

Build variants:

| Command | Result |
|---|---|
| `make` | Real build: GTK4 + layer-shell overlay, libmpv video (`./cctv-overlay`). |
| `make STUB=1` | Simulated video and overlay, no GTK or mpv needed. Useful for testing the control interface on a machine without a display. |
| `make check` | Unit tests and the control-interface test against the stub build. Headless. Needs `jq`. |
| `make check-real` | Opt-in test of the real build on a live Wayland session. Shows the overlay briefly on `CCTV_TEST_MONITOR` (default `DP-1`); uses a local loopback RTSP server with fake credentials and never reads your configuration. |

## 3. Install

### Option A: user-level install (no root)

```sh
make PREFIX="$HOME/.local" install
```

This puts `cctv-overlay` in `~/.local/bin` (make sure that is on your `PATH`). Remove it with
`make PREFIX="$HOME/.local" uninstall`.

### Option B: system-wide install (root)

```sh
sudo make PREFIX=/usr/local install
```

The install step writes:

| Path | Content |
|---|---|
| `$(PREFIX)/bin/cctv-overlay` | The program |
| `$(PREFIX)/share/doc/cctv-overlay/config.example.ini` | Config template |
| `$(PREFIX)/share/doc/cctv-overlay/secrets.example.ini` | Secrets template |
| `$(PREFIX)/share/doc/cctv-overlay/behaviour.md`, `overlay.md` | Documentation |

Uninstall with the same `PREFIX` and `sudo make PREFIX=/usr/local uninstall`. Uninstall does not touch
`~/.config/cctv-overlay`.

### Option C: Arch package (PKGBUILD)

The draft in [`packaging/PKGBUILD`](../packaging/PKGBUILD) builds from a git checkout of this repository. From
a checkout:

```sh
cd packaging
makepkg -si
```

It installs the binary, the documentation, the systemd user unit (`/usr/lib/systemd/user/cctv-overlay.service`)
and the Hyprland snippets under `/usr/share/doc/cctv-overlay/`. The package licence is still undecided
(`custom:TBD`); see the README.

## 4. Create your configuration

Copy the templates. `cp -n` never overwrites an existing file. The secrets file is created with mode `0600`
from the start.

```sh
mkdir -p ~/.config/cctv-overlay
cp -n /usr/share/doc/cctv-overlay/config.example.ini ~/.config/cctv-overlay/config.ini
( umask 077 && cp -n /usr/share/doc/cctv-overlay/secrets.example.ini ~/.config/cctv-overlay/secrets.ini )
```

For a user-level install, the templates are in `$PREFIX/share/doc/cctv-overlay/` (for example
`~/.local/share/doc/cctv-overlay/`), or in `examples/` in the source tree.

Then edit both files:

1. `config.ini`: set `[recorder] host` (your recorder's LAN address), `username`, and the `[camera.*]` sections
   with the correct `channel` numbers.
2. `secrets.ini`: replace `change-me` with the real password. Do not paste the password into a shell command.

Check the file modes:

```sh
ls -l ~/.config/cctv-overlay/
```

`secrets.ini` must show `-rw-------`.

## 5. Run

In a terminal, to see logs:

```sh
cctv-overlay daemon
```

From another terminal (or a key binding):

```sh
cctv-overlay list
cctv-overlay toggle
cctv-overlay status
cctv-overlay toggle   # hide
```

Any client command starts the daemon automatically if it is not running, but then its log is not visible.
For a persistent daemon, use the systemd user unit below.

### Optional: systemd user unit

The unit runs the daemon in your user session. It is **not** enabled by installation.

```sh
cp -n packaging/cctv-overlay.service ~/.config/systemd/user/    # or /usr/lib/systemd/user/ from the package
systemctl --user daemon-reload
systemctl --user enable --now cctv-overlay.service
journalctl --user -u cctv-overlay.service -f
```

The unit starts `/usr/bin/cctv-overlay daemon`. If you installed to another prefix (for example
`~/.local`), change `ExecStart` in your copy of the unit to the full path of the binary. The unit verifies
cleanly with `systemd-analyze verify`, but it has not been run on a live session yet.

The unit needs `WAYLAND_DISPLAY` in the user manager's environment. Most compositors export it; if the
daemon exits with `overlay unavailable` under systemd, check `systemctl --user show-environment | grep WAYLAND`
and import it from your compositor's startup (for example `systemctl --user import-environment WAYLAND_DISPLAY`).

To stop the unit and disable it: `systemctl --user disable --now cctv-overlay.service`.

## 6. Bind keys

cctv-overlay does not choose keys. Bind the commands in your compositor. Examples, with placeholder keys
you should change: [`docs/hyprland.md`](hyprland.md). Any tool that can run a command works.

## Troubleshooting

See [`troubleshooting.md`](troubleshooting.md).
