# Architecture — CCTV Overlay

**Status:** proposed, pending the Milestone 1 spike. Decisions marked *provisional* are expected to be
confirmed or changed by measured evidence, not preference.

## 1. Summary

A single user-level daemon (`cctv-overlay daemon`) owns one GTK4 layer-shell window and one libmpv player
instance. A thin CLI sends JSON commands over a Unix socket. The daemon keeps no player and no network
connection while hidden; on show it fetches an optional JPEG preview and opens the RTSP sub-stream with
hardware decode, then renders into the overlay through libmpv's OpenGL render API.

```
┌────────────────────────────── cctv-overlay daemon ──────────────────────────────┐
│ GTK4 main loop                                                                  │
│                                                                                 │
│  ┌────────────────┐      ┌──────────────────┐      ┌────────────────────────┐   │
│  │ Control server │─────▶│ State machine    │─────▶│ Overlay window         │   │
│  │ Unix socket    │      │ hidden/connecting│      │ GtkWindow + layer-shell│   │
│  │ JSON lines     │◀─────│ live/reconnect   │◀─────│ GtkGLArea + label      │   │
│  └────────────────┘      └────────┬─────────┘      └───────────┬────────────┘   │
│                                   │                            │                │
│  ┌────────────────┐      ┌────────▼─────────┐      ┌───────────▼────────────┐   │
│  │ Config/secrets │─────▶│ Camera manager   │─────▶│ Video engine (libmpv)  │   │
│  │ GKeyFile 0600  │      │ ordered list     │      │ render API, VA-API     │   │
│  └────────────────┘      └──────────────────┘      └───────────┬────────────┘   │
│                                                                │                │
│  ┌────────────────┐                                            │ RTSP/TCP       │
│  │ Snapshot worker│──────── HTTP Digest ───────────────────────┼──▶ Dahua NVR  │
│  │ libcurl        │                                            │                │
│  └────────────────┘                                            │                │
└─────────────────────────────────────────────────────────────────────────────────┘
              ▲
              │ $XDG_RUNTIME_DIR/cctv-overlay/control.sock (0600)
┌─────────────┴──────────────┐
│ cctv-overlay CLI           │ toggle | next | prev | show | hide | status |
│ (same binary, client mode) │ list | reload | quit
└────────────────────────────┘
```

## 2. Why this shape

### 2.1 Overlay: layer-shell, not a normal window

| Option | Verdict |
|---|---|
| **wlr-layer-shell `overlay` layer** | **Chosen (provisional).** No decorations; compositor-managed stacking above windows; `keyboard_interactivity=none`; empty input region gives click-through; no workspace or focus changes. Supported by Hyprland/Sway/wlroots, KDE Plasma Wayland, Mir, COSMIC; not GNOME or X11. |
| Normal floating window + Hyprland rules | Rejected for v0.1. A toplevel cannot be made click-through portably, usually takes focus, and fullscreen windows cover it. Usable as a documented fallback with reduced guarantees. |
| Hyprland plugin rendering inside the compositor | Rejected. Best integration, worst maintenance: private ABI, breaks on Hyprland updates, unusable by other compositors. |
| Special workspace / pinned window | Rejected. Does not stay above fullscreen content and changes workspace semantics. |
| Screen-copy mirroring / fake overlay | Rejected. Adds a full-screen capture and composition cost for a small PiP. |

`wlr-layer-shell` is the closest equivalent to the Android accessibility overlay in the reference
project: a compositor-level surface that never participates in normal focus/tiling.

### 2.2 Video: libmpv render API, not a separate player window

libmpv 2.5.0 is installed with `client.h`, `render.h` and `render_gl.h`. Embedding the render API into a
`GtkGLArea`:

- avoids a second toplevel and therefore keeps the overlay atomic;
- reuses mpv's mature RTSP/HEVC/VA-API handling, reconnect-ish behaviour and low-latency profile;
- lets us free the entire player on hide, so hidden means no decode and no network;
- keeps the video layer replaceable behind a small `video_engine` interface.

Rejected alternatives:

| Alternative | Why not now |
|---|---|
| GStreamer + `gtk4paintablesink` | Clean GTK integration, but this system lacks `gtk4paintablesink` and the VA decoders (`vah264dec`/`vah265dec`); requires package installs and more reconnect/state code of our own. Keep as fallback if libmpv/GL integration fails. |
| mpv in its own window with `--wid` | `--wid` embedding is X11/Windows only; no Wayland equivalent. |
| FFmpeg decode + custom GL upload | Reimplements decoding, sync, buffering and reconnect. Too much for v0.1. |
| VLC/libVLC | Heavier embedding API, no advantage here. |

## 3. Components

### 3.1 `config`
- Loads `$XDG_CONFIG_HOME/cctv-overlay/config.ini` (default `~/.config/cctv-overlay/config.ini`) via
  `GKeyFile`; `--config` overrides for tests.
- Loads `secrets.ini` next to it (auto-detected) or `CCTV_OVERLAY_PASSWORD` / `CCTV_OVERLAY_USERNAME`
  environment overrides.
- Validates types, ranges, required fields, duplicate names, anchor values, layer values, URL overrides.
- Camera order = group order in the file; cycling follows that order.
- Never copies the password into a struct that is logged or printed; the RTSP URL is built only at connect
  time in a scope that is wiped/freed immediately after `loadfile`.

See §7 for the schema.

### 3.2 `camera_manager`
- Ordered `Camera[]`: name, channel, optional subtype, optional full URL override.
- Current index, `next`/`prev` with wrap.
- `resolve_url(camera)` builds
  `rtsp://<user>:<pass>@<host>:<port><path>?channel=<n>&subtype=<s>` with RFC 3986 percent-encoding of
  userinfo; credentials never appear in config-visible URLs or status output.
- `probe` uses a disposable mpv instance to report codec/resolution/fps and, when credentials allow, the
  Dahua channel-title CGI to list configured channels.

### 3.3 `video_engine`
- One `mpv_handle` + one `mpv_render_context` per active stream.
- Options (initial set, to be tuned by measurement):
  - `vo=libmpv`
  - `hwdec=vaapi` (fallback `vaapi-copy`, then `auto-safe`)
  - `profile=low-latency`
  - `rtsp-transport=tcp` (configurable `udp`)
  - `audio=no`, `ao=null`
  - `cache=no`, `demuxer-readahead-secs=0`
  - `network-timeout=3`
  - `keep-open=no`, `idle=yes`, `force-window=no`
  - log level `warn`, no terminal output, no URL logging
- Events on the mpv thread are marshalled to the GTK main loop with `g_idle_add`; the render update
  callback only schedules a redraw.
- First frame = first successful `mpv_render_context_render` after `loadfile`; timestamp recorded.
- Stop path: `mpv_command("stop")` → `mpv_render_context_free` → `mpv_terminate_destroy` (joins the
  thread) → clear FBO/texture state. No player survives `hide`.
- Reconnect: on `MPV_EVENT_END_FILE` with error, retry 0.5/1/2/4/8 s, max 5; state `reconnecting`;
  a stall watchdog (no frame for 10 s while visible) forces one restart; user action resets backoff.

### 3.4 `overlay`
- `GtkApplicationWindow` + `gtk_layer_init_for_window`.
- `layer=overlay` (configurable `top`), `namespace=cctv-overlay`, anchors and margins from config,
  `exclusive_zone=-1` so panel exclusive zones do not shift it, keyboard mode `NONE`.
- **Click-through:** after realize, set an empty input region
  (`gdk_surface_set_input_region(surface, empty_cairo_region)`); reapply on map and size change. If GDK's
  setter proves unreliable, fall back to `wl_surface_set_input_region` through
  `gdk_wayland_surface_get_wl_surface()` (headers available).
- No focus: keyboard mode NONE, `gtk_window_set_focusable(FALSE)`, no `gtk_window_present` focus steal;
  never call `gtk_window_set_active`.
- Monitor: config `focused | primary | <connector>`; resolved with GDK monitor geometry (handles DP-1's
  negative origin). "Focused" resolution is resolved at show time; an explicit connector is the
  predictable option for scripts.
- Layout: `GtkOverlay` → `GtkGLArea` + label/status box; CSS class with 10 px radius, 1 px neutral border,
  small padding; black letterbox because video alpha is not guaranteed by the GL path. No animation.
- Size: config `width`/`height` (default 480×270); video fitted with aspect preserved; label shows camera
  name, status dot shows connecting/live/reconnecting/error.
- Hide: unmap, drop preview texture, free engine. No window stays mapped while hidden.

### 3.5 `control` (IPC)
- `GSocketService` on a Unix socket at `$XDG_RUNTIME_DIR/cctv-overlay/control.sock`; runtime dir 0700,
  socket 0600; `SO_PEERCRED` check that the peer UID equals ours.
- One JSON object per line, request and response; max 4 KiB; per-connection timeout 2 s.
- Commands: `toggle`, `next`, `prev`, `show {camera}`, `hide`, `status`, `list`, `reload`, `quit`, `ping`.
- Responses always include `ok`, `version`; `status` adds `visible`, `state`, `camera`, `camera_index`,
  `latency` (last preview/live ms), `uptime_ms`, `error` (sanitized).
- Single instance via `flock` on `daemon.lock`; a second daemon exits with a clear error.

### 3.6 `cli`
- Same binary; `cctv-overlay <command>` acts as client, `cctv-overlay daemon` runs the daemon.
- If the socket is absent, the CLI spawns the daemon (detached, same config) and waits ≤2 s for the socket;
  `--no-spawn` disables this for tests. A systemd user unit is the recommended always-warm setup.
- Exit codes: 0 success, 1 command failure, 2 usage/config error, 3 daemon unavailable, 4 timeout.
- `--json` for machine-readable output; `status` also prints a short human line by default.
- Credentials never appear in CLI arguments.

### 3.7 `snapshot` (preview)
- Optional (default on). Fetch `http://<host>/cgi-bin/snapshot.cgi?channel=<n>` with libcurl
  `CURLAUTH_DIGEST`, timeouts 1.5 s connect / 2 s total, max 4 MB.
- Runs in a worker thread (`GTask`); the JPEG becomes a `GdkTexture` shown immediately, then replaced by
  the first live frame.
- The snapshot is cached per camera in memory for the session and reused as a placeholder during the next
  switch (no disk writes).
- If it fails, the overlay shows a neutral "connecting" state; live playback is unaffected.

## 4. State machine

```
                  toggle/show                  first frame
   HIDDEN ─────────────────────▶ CONNECTING ───────────────▶ LIVE
     ▲                               │  ▲                      │
     │ hide/quit                     │  │ retry (≤5)           │ stream error / stall
     │                               ▼  │                      ▼
     └──────────────────────────  RECONNECTING ◀───────────────┘
                                     │ retries exhausted
                                     ▼
                                   ERROR ── toggle/show ──▶ CONNECTING
```

- `SWITCHING` is a sub-state of CONNECTING with a preview texture visible; status reports `connecting`
  plus `preview=true` to keep the public state set small.
- `next`/`prev` while HIDDEN: **open the overlay on the next camera** (chosen UX). Rationale: a camera
  action without visual feedback is ambiguous; opening gives immediate confirmation and matches the
  reference project's stepping behaviour. `show <camera>` also opens; `status` remains the way to inspect
  without opening.
- `hide` during CONNECTING cancels the pending connection and frees the player.
- `reload` re-reads config; if the current camera still exists, keep it; otherwise select the first and
  reconnect only if visible.

## 5. Latency budget

| Stage | Typical (design target) | Notes |
|---|---|---|
| CLI → daemon (Unix socket) | < 5 ms | Local, no serialization overhead worth counting |
| State change → overlay mapped | 5–20 ms | Wayland commit + compositor frame |
| Snapshot HTTP (LAN, Digest) | 80–250 ms | Preview path; depends on camera |
| RTSP TCP connect + DESCRIBE/SETUP/PLAY | 60–200 ms | Wired LAN, direct |
| Wait for next IDR | 0–2000 ms | Dominated by camera GOP; sub-stream usually short |
| Decode + first render | 10–40 ms | VA-API, 704×576 |
| Present to screen | 6–16 ms | One refresh at 60–165 Hz |
| **Toggle → preview** | **≤ 250 ms** | Snapshot-first |
| **Toggle → live** | **0.3–1.5 s** | Target p95 ≤ 1.2 s |
| **Switch → live** | **0.2–1.0 s** | Cached snapshot covers the gap |

Switch strategies to measure in M5:

1. **Single instance + `loadfile`** (default): simple, one connection, ~RTSP handshake + IDR wait.
2. **Snapshot placeholder + single instance**: same live timing, but the user sees the target camera
   immediately; best perceived latency for the least complexity.
3. **Preloaded spare player** for the next camera: fastest live frame, but a second connection and decode
   risk; config `preload_next=false` by default until measurements justify it.

## 6. Performance and resource model

- Hidden: no mpv instance, no RTSP socket, no timers except the IPC service and an idle GTK loop.
- Visible: one HEVC/H.264 sub-stream decode on VCN; one GL texture; one 480×270 composite. Expect a few
  percent of one core and modest RSS; measure rather than assert.
- Overlay presence disables direct scanout for a fullscreen game (the compositor must composite). This is
  an expected cost of any overlay; measure the fps impact and document it. If the cost is unacceptable,
  the user can hide the overlay during competitive play; do not silently alter compositor settings.
- No polling loops: all state changes are event-driven.

## 7. Configuration schema (draft)

`~/.config/cctv-overlay/config.ini` (0644, no secrets):

```ini
[general]
# log_level = info            ; debug|info|warning|error
# monitor = focused           ; focused|primary|<connector e.g. DP-1>
# start_hidden = true

[overlay]
width = 480
height = 270
anchor = top-right            ; top-left|top-right|bottom-left|bottom-right
margin_x = 24
margin_y = 24
layer = overlay               ; overlay|top
show_label = true
show_status = true
# opacity = 1.0               ; 0.1..1.0 (informational; layer opacity is compositor-dependent)

[stream]
transport = tcp               ; tcp|udp
subtype = 1                   ; 0=main, 1=sub (default sub for latency/bandwidth)
hwdec = vaapi                 ; vaapi|vaapi-copy|auto-safe|no
preview_snapshot = true
preload_next = false
reconnect = true

[recorder]
host = 192.168.1.100          ; placeholder; set to your recorder
port = 554
username = cctv
path = /cam/realmonitor

[camera.front]
name = Front Door
channel = 1

[camera.driveway]
name = Driveway
channel = 2
# subtype = 0                 ; per-camera override

# Any RTSP source can bypass the Dahua URL builder:
# [camera.other]
# name = Other
# url = rtsp://user:pass@host:554/path   ; discouraged: contains a secret
```

`secrets.ini` (0600, gitignored, owner-created):

```ini
[recorder]
password = change-me
# username = cctv              ; optional override
```

Precedence: `CCTV_OVERLAY_PASSWORD` / `CCTV_OVERLAY_USERNAME` > `secrets.ini` > error if missing.
The app refuses to start if the password is missing; it warns (not fails) if `secrets.ini` is group/other
readable, and names the exact file and mode in the warning.

## 8. Security model

- **No credentials in Git.** `secrets.ini`, `config.ini` and `.env` are ignored; the example config has an
  empty/absent password and comments explain the two-file model.
- **No credentials in argv.** The daemon is started with a config path only; RTSP userinfo exists only in
  a heap string immediately before `loadfile`, then freed.
- **No credentials in logs.** A central logger redacts `scheme://user:pass@` and any registered secret
  substring before writing; mpv log messages are filtered and redacted; URL is never logged whole.
- **No credentials in status/IPC.** `status` reports camera names, channels, host and state only.
- **File modes.** Config dir 0700 if created by the app; secrets 0600; socket 0600; peer UID checked.
- **No network exposure.** No TCP listener, no HTTP server, no cloud calls. The only outbound connections
  are to the configured recorder and its snapshot endpoint.
- **No remote code.** No plugin loading, no shelling out with user data, no downloaded binaries.
- **Crash dumps** may contain the URL; document `ulimit -c 0` or the user's coredump policy as a
  hardening note rather than silently changing system settings.
- **IPC trust boundary:** same-UID only. Any process running as the user can already read the secrets file,
  so a token would add complexity without a real boundary.

## 9. Portability

| Environment | Layer shell | Click-through | Above fullscreen | Plan |
|---|---|---|---|---|
| Hyprland (target) | Yes | Yes (to verify) | To verify in M1 | Primary target |
| Sway / wlroots | Yes | Yes (to verify) | To verify | Documented/test if available |
| KDE Plasma Wayland | Yes (per gtk4-layer-shell) | To verify | To verify | Documented |
| GNOME Wayland | No | No | No | Fallback: normal undecorated window, always-on-top hint; document reduced guarantees |
| X11 | No | X11 shape possible later | Compositor-dependent | Not supported in v0.1 |

The core (config, camera model, IPC, video engine) has no Hyprland or Omarchy dependency. Only the overlay
backend is compositor-specific; `--no-layer-shell` selects the fallback window.

## 10. Repository layout (planned)

```
cctv-overlay/
├── Makefile
├── README.md
├── LICENSE                     (TBD)
├── CHANGELOG.md
├── PLAN.md  ARCHITECTURE.md  DELEGATION.md
├── src/
│   ├── main.c                  entry: daemon or CLI
│   ├── app.c/h                 state machine, wiring
│   ├── config.c/h              GKeyFile load/validate
│   ├── camera.c/h              ordered cameras, URL builder
│   ├── ipc.c/h                 Unix socket server/client
│   ├── overlay.c/h             layer-shell window, GL area, CSS
│   ├── video.c/h               libmpv render API engine
│   ├── snapshot.c/h            libcurl Digest snapshot
│   ├── logging.c/h             redaction, levels
│   └── util.c/h                strings, time, percent-encoding
├── tests/
│   ├── test_config.c
│   ├── test_util.c
│   └── test_ipc.sh
├── examples/
│   ├── config.example.ini
│   └── secrets.example.ini
├── integrations/hyprland/
│   ├── example.lua             Omarchy 4.5 / Hyprland Lua config
│   └── example.conf            classic Hyprland config
├── packaging/
│   ├── PKGBUILD
│   └── cctv-overlay.service
├── docs/
│   ├── install.md
│   ├── configuration.md
│   ├── hyprland.md
│   └── troubleshooting.md
└── scripts/
    ├── dev-rtsp.sh             local ffmpeg RTSP harness
    └── check-latency.sh
```

## 11. Open technical questions (M1 spike)

| # | Question | Method | Blocks |
|---|---|---|---|
| S1 | Does Hyprland 0.56.2 draw an `overlay`-layer surface above a fullscreen window in modes 1/2/3, native fullscreen, gamescope and Proton? | Python GTK4 layer-shell test pattern + `grim` pixel check + `hyprctl activewindow` | Whole design (R1) |
| S2 | Does the overlay disable direct scanout, and what does that cost in a fullscreen game? | Sample GPU/fps with overlay on/off; document | Performance expectations |
| S3 | Does an empty input region give real click-through, and does keyboard stay with the underlying app? | `wtype`, `hyprctl activewindow`, manual click test | A5 |
| S4 | Does libmpv render into `GtkGLArea` with VA-API correctly (colour, flip, alpha, no tearing)? | Minimal C spike with `testsrc` and the real camera | M3/M4 (R2) |
| S5 | Is `gdk_surface_set_input_region` reliable across map/resize? | Inspect behaviour; fallback to `wl_surface_set_input_region` | Click-through |
| S6 | Which low-latency option set and transport (TCP/UDP) gives the best measured result? | A/B 20 runs each, p50/p95 | M5 targets |
| S7 | Is the Dahua snapshot endpoint available, and how fast? | libcurl probe with credentials | Preview design |
| S8 | How many simultaneous RTSP sessions does the NVR allow? | Two-client test with credentials | Preload decision |
| S9 | Does `focused` monitor resolution work without Hyprland-specific calls? | Test on DP-1/DP-2, negative origin | Multi-monitor |
| S10 | Does CSS rounded clipping work on `GtkGLArea`, and is alpha available? | Visual test + screenshot | Visual polish only |

## 12. Decision records

Public ADRs live in `docs/adr/`:

- `0001-overlay-surface.md` — layer-shell vs normal window/plugin (**proposed**).
- `0002-video-engine.md` — libmpv render API vs GStreamer (**proposed**).
- `0003-control-interface.md` — Unix socket + CLI (**proposed**).

They move to *Accepted* only after the M1 evidence is recorded in `.agent-work/TESTS.md`.
