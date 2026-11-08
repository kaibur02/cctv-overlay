# cctv-overlay — video and overlay (M3/M4)

The default build shows the camera in a small layer-shell window and decodes it with libmpv. This page
covers how that works and what to check; `behaviour.md` covers commands and the state machine.

## Building

| Command | Result |
|---|---|
| `make` | Real build: GTK4 + gtk4-layer-shell overlay, libmpv video (`./cctv-overlay`). |
| `make STUB=1` | Simulated video/overlay, no GTK/mpv needed. Switching between the two relinks automatically. |
| `make check` | Unit tests plus `tests/test_ipc.sh` against the **stub** binary (`build/stub/cctv-overlay`). Headless. |
| `make check-real` | Opt-in `tests/test_real.sh` against the real build. Needs a Wayland session (Hyprland: `hyprctl`), `grim`, `jq`, `ffmpeg`, `python3`, `ss`. Shows the overlay briefly on `CCTV_TEST_MONITOR` (default `DP-1`), loopback network only, fake credentials. |

Build dependencies (pkg-config): `gtk4`, `gtk4-layer-shell-0`, `mpv` (libmpv), `epoxy`, plus the GLib set.

**Link order matters.** `gtk4-layer-shell` must be loaded before libwayland, otherwise GTK silently makes
a normal toplevel instead of a layer surface. The Makefile puts `-lgtk4-layer-shell` first (check with
`readelf -d cctv-overlay | grep NEEDED`: it must be the first entry). No `LD_PRELOAD` is used. At
startup the daemon verifies this (a probe window must really become a layer surface) and **fails
loudly** with exit code 1 and a message such as `overlay unavailable: ...` instead of drawing a normal
window. There is deliberately no fallback to a plain window.

## The overlay window

- Layer-shell surface, namespace `cctv-overlay`, layer from `[overlay] layer` (`overlay` by default;
  see below for `top`), keyboard mode `none`, exclusive zone -1, no decorations, not focusable.
- Size, anchor and margins come from `[overlay] width`, `height`, `anchor`, `margin_x`, `margin_y`.
  `margin_x` applies to the anchored left/right edge, `margin_y` to the anchored top/bottom edge.
- **Click-through:** an empty input region is applied when the window is realised, mapped and on every
  resize, and the widgets are non-targetable. Pointer events go to the window below.
- It is shown without taking focus (it is mapped with `gtk_widget_set_visible`, not `present()`), and
  it is not a client window (`hyprctl clients` does not list it; `hyprctl layers` does, at level 3).
- Content: black background, video letterboxed to keep its aspect ratio, a small label chip (status
  dot + camera name) at the top left, a status line at the bottom when not live. Corners are rounded
  with CSS (`border-radius` + `overflow: hidden`). No animation. `[overlay] show_label` and
  `show_status` hide the chip parts; `opacity` is not applied.
- Status dot: grey/amber = connecting or reconnecting, green = live, red = error. The status line shows
  `Connecting…`, `Reconnecting… <reason>` or `Error: <reason>`; reasons never contain URLs or credentials.

### Layer `overlay` vs `top`

Use `overlay` (the default). Hyprland 0.56 fades `top` layers out under fullscreen windows (decision
D-018). `top` is accepted for compositors where that is wanted. A visible overlay also stops direct
scanout of a fullscreen window on that monitor while it is shown.

### Choosing the monitor

`[general] monitor`:

- `<connector>` (for example `DP-1`) is the reliable choice: the overlay always appears on that output.
  If the connector does not exist, a warning is logged and the compositor chooses.
- `focused`: GTK has no portable API for the focused monitor, so the output is left unset and the
  compositor decides. On Hyprland this is the focused monitor at the moment of showing; on other
  compositors it may differ. Use a connector name if you need certainty.
- `primary`: the first monitor GDK lists (GTK 4 has no real "primary").

## Video engine

One libmpv instance plus render context per session, drawing into the overlay's `GtkGLArea`
(`vo=libmpv`, hardware decode from `[stream] hwdec`, default `vaapi`; mpv falls back to software when
the hardware path is unusable). Options: `profile=low-latency`, `cache=no`,
`demuxer-readahead-secs=0.2`, no audio, `rtsp-transport` from `[stream] transport`, RTSP socket timeout
`demuxer-lavf-o=…timeout=5000000` (5 s in microseconds; `network-timeout` is ignored for RTSP),
mpv scripts/config/ytdl/OSC disabled, terminal output off.

- **Threads:** everything runs on the GTK/GLib main thread. Two tiny foreign-thread callbacks (mpv wakeup,
  render update) only set atomics and `g_idle_add`. There is no event thread to join.
- **Fresh player per session.** Creating mpv plus the render context measured about 12-14 ms, and
  tearing it down about 1-3 ms (idle) to 60 ms (while a connect is hanging), against 250-400 ms to the first
  frame, which is dominated by RTSP set-up and the wait for a key frame. Reusing a handle would save
  a few percent and complicate "hidden means no player", so each `video_start` creates a new one and
  `video_stop` destroys it.
- **Teardown order** (from the M1 spike): free the render context with the GL context current, then
  `mpv_terminate_destroy`. `overlay_hide()` stops the video first, then destroys the window.
- **Hidden means nothing held:** no mpv handle, no render context, no GL context, no RTSP socket. The glibc
  arena count is capped at 2 (`mallopt`) and `malloc_trim` runs 300 ms after hiding, because per-thread
  arenas otherwise ratchet RSS upward across show/hide cycles (this was measured: not a leak).
- **Events:** first rendered frame → `first_frame`; end-file with error → `error` (message is mapped to
  a fixed sentence such as `authentication failed (check username and password)`, `connection refused`,
  `could not open the stream`); clean EOF → `ended`; a watchdog reports `error` when no new frame arrives
  for 10 s while live, or 15 s before the first frame. Retry/backoff stays in `app.c`.
- **Secrets:** the credentialed URL exists only for the `loadfile` call (argv array via
  `mpv_command_async`), then it is wiped. mpv log messages (warn and above only) pass through
  `log_redact()`, are limited to 40 lines per session, and mpv's own terminal output is disabled.
  Known limit: mpv keeps the URL in its process memory while a stream plays.
- A locale quirk: GTK sets `LC_NUMERIC` from the environment, libmpv refuses to start unless it is `C`,
  so the engine resets it before creating the player.

## Manual checks still needed (not covered by `make check-real`)

1. Overlay visible above a **real fullscreen window** on the chosen monitor (layer `overlay`).
2. Clicks, scroll and keyboard go to the window below (empty input region); focus never moves.
3. A real recorder (Dahua) stream: auth, channel numbering, H.264/H.265 and key-frame wait.
4. `monitor = focused` on a multi-monitor session (compositor dependent).
5. Behaviour when the output is unplugged while visible: the window is destroyed by the compositor; the
   engine reports `overlay surface was lost` and the app retries. The recovery path is covered by the stub test
`/app/overlay-lost-while-live`; a real monitor unplug is still a manual check.
