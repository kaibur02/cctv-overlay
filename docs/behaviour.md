# cctv-overlay — behaviour

`cctv-overlay` is one binary. `cctv-overlay daemon` runs the long-lived process that owns the overlay
window and the video player; every other command is a thin client that sends one request to the daemon
over a Unix socket. The default (real) build shows the camera in a layer-shell overlay decoded with libmpv.
Video and layout details are in [`overlay.md`](overlay.md); this page covers commands, states and
configuration semantics.

## Commands

| Command | Effect |
|---|---|
| `daemon [--config PATH] [--no-overlay]` | Run the daemon in the foreground. The daemon always starts hidden. |
| `toggle` | Hidden → open on the current camera. Visible (connecting/live/reconnecting) → hide. In the error state, toggle retries instead of hiding; use `hide` to close. |
| `next` / `prev` | Cycle cameras in config-file order, wrapping around. **While hidden they also open the overlay on the neighbouring camera.** While visible they switch to the new camera. |
| `show [CAMERA]` | Open on CAMERA (name, case-insensitive; or id; or zero-based index). Without an argument, open on the current camera. If it is already showing, nothing happens (unless it is in error or reconnecting, in which case it retries now). |
| `hide` | Close the overlay from any state. Stops the stream, so nothing is decoded or connected while hidden. |
| `status [--json]` | State, camera, index, count, last error. Never prints credentials or URLs. |
| `list` | Configured cameras (`*` marks the current one). |
| `reload` | Re-read `config.ini` and `secrets.ini`. Keeps the current camera if it still exists (matched by id); otherwise selects the first one and reconnects only if visible. An invalid file is rejected and the old configuration stays active. |
| `quit` | Stop the daemon and remove the socket. |
| `ping` | Health check. Prints the daemon version (`--json` shows the raw reply). |

Global options: `-c/--config PATH`, `--socket PATH`, `--no-spawn`, `--json`, `--version`, `-h/--help`.

If the daemon is not running, any client command except `quit` starts it (detached, with only the config
path on its command line) and waits up to 2 s for the socket. `--no-spawn` disables that. An auto-started
daemon has no terminal, so its log goes nowhere useful; for logs run `cctv-overlay daemon` in a terminal or
use the systemd user unit (see [`install.md`](install.md)).

### States

`hidden → connecting → live`. Stream errors or stalls go to `reconnecting` (retries after 0.5, 1, 2, 4 and 8 s,
at most 5 attempts), then to `error`. `hide` always returns to `hidden`. If the stream cannot even be started
(for example no password configured), the state is `error` immediately, without retries.

### Exit codes

| Code | Meaning |
|---|---|
| 0 | Success |
| 1 | The command failed (unknown camera, invalid reload, a daemon is already running, overlay unavailable at daemon start, …) |
| 2 | Usage or configuration error |
| 3 | Daemon unavailable (not running and could not be started, or `--no-spawn`) |
| 4 | Timed out waiting for the daemon (2 s) |

### `status --json`

Example reply (values are illustrative):

```json
{"ok":true,"visible":true,"state":"live","camera":"Front Door","camera_index":0,
 "camera_count":2,"error":null,"retries":0,"uptime_ms":4210}
```

`error` is `null` unless the last stream error is set. The reply never contains the RTSP URL, the
username or the password.

## Configuration

The daemon reads `$XDG_CONFIG_HOME/cctv-overlay/config.ini` (default `~/.config/cctv-overlay/config.ini`);
`--config` overrides it. All keys are described in [`configuration.md`](configuration.md). Key points:

- Values are strictly validated. Errors name the file, section and key.
- Camera order is the order of the `[camera.<id>]` sections; names must be unique (case-insensitive).
- `channel` ≥ 1 is required unless the camera has an `rtsp://` `url` override.
- `[recorder] host` has no default; it is required unless every camera has a `url`.
- Unknown sections and keys are errors (this catches typos). A `password` key in `config.ini` is an error on purpose.
- A `password` for the recorder lives in `secrets.ini` (or the environment), never in `config.ini`.

A missing password is **not** a startup error: the daemon, `list` and `status` work, and the error appears
only when a stream is about to start.

## Security model

- Credentials live only in `secrets.ini` (keep it mode `0600`; a warning names the file and mode if group or
  other can read it) or in the environment (`CCTV_OVERLAY_PASSWORD`, `CCTV_OVERLAY_USERNAME`). They are never
  on a command line.
- The RTSP URL (with percent-encoded credentials) is built only at connect time, in a heap buffer that is
  wiped after use. It is not stored in configuration structures, status output, logs or error messages.
- All log output and error replies pass through a redactor: `scheme://user:pass@` becomes
  `scheme://***:***@`, and the registered password (raw and percent-encoded) becomes `***`.
- The control socket is `$XDG_RUNTIME_DIR/cctv-overlay/control.sock` (directory `0700`, socket `0600`), and
  peers are checked with `SO_PEERCRED` so only the same UID is accepted. Requests are one JSON line of at most
  4 KiB with a 2 s timeout. There is no network listener.
- One daemon per user (`flock` on `daemon.lock` next to the socket). The socket is removed on `quit`, SIGTERM and SIGINT.
- `make install` only writes the binary and example files under `$(PREFIX)`. It never creates or overwrites
  anything in your home configuration directory.
- Known limit: the password is held in process memory while the daemon runs, and GLib parser buffers and
  mpv's memory may hold it briefly. A core dump can therefore contain it. Disable core dumps for the daemon
  if that matters to you (`ulimit -c 0` in the shell that starts it, or `LimitCORE=0` in the systemd unit).

## Settings that are accepted but do nothing yet

These keys are parsed and validated, but the current build does not act on them:

- `[general] start_hidden`: the daemon always starts hidden.
- `[stream] preview_snapshot`, `[stream] preload_next`: no snapshot preview and no preload yet.
- `[overlay] opacity`: not applied.

See [`configuration.md`](configuration.md) for the full list.

## Test knobs

For development only. The stub build (`make STUB=1`) reads `CCTV_OVERLAY_STUB_FAIL` (`1`, `always` or
`first:N`), `CCTV_OVERLAY_STUB_DELAY_MS`, `CCTV_OVERLAY_STUB_DROP_AFTER_MS`. Both builds read
`CCTV_OVERLAY_BACKOFF_SCALE` to shorten the reconnect delays in tests. Do not rely on these in normal use.
