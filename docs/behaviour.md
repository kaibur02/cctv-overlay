# cctv-overlay — behaviour (M2 headless core)

`cctv-overlay` is one binary: `cctv-overlay daemon` runs the long-lived process, every other command is a
thin client that talks to it. In this milestone video and overlay are **simulated** (stubs), so the
state machine, configuration and control interface can be used and tested without a screen or camera.

## Commands

| Command | Effect |
|---|---|
| `daemon [--config PATH] [--no-overlay]` | Run the daemon in the foreground. |
| `toggle` | Hidden → open on the current camera. Visible (connecting/live/reconnecting) → hide. In the error state, toggle retries instead of hiding; use `hide` to close. |
| `next` / `prev` | Cycle cameras in config-file order, wrapping around. **While hidden they also open the overlay on the neighbouring camera.** While visible they reconnect to the new camera. |
| `show [CAMERA]` | Open on CAMERA (name, case-insensitive; or id; or zero-based index). Without an argument, open on the current camera. Already showing it: nothing happens (unless in error/reconnecting, then it retries now). |
| `hide` | Close the overlay from any state. Stops the stream, so nothing is decoded or connected while hidden. |
| `status [--json]` | State, camera, index, count, last error. Never prints credentials or URLs. |
| `list` | Configured cameras (`*` marks the current one). |
| `reload` | Re-read `config.ini`/`secrets.ini`. Keeps the current camera if it still exists (by id); otherwise selects the first and reconnects only if visible. An invalid file is rejected and the old configuration stays active. |
| `quit` | Stop the daemon (removes the socket). |
| `ping` | Health check. |

Global options: `--config PATH`, `--socket PATH`, `--no-spawn`, `--json`, `--version`.

If the daemon is not running, any client command except `quit` starts it (detached, with only the config
path on its command line) and waits up to 2 s for the socket. `--no-spawn` disables that. The
auto-started daemon has no terminal; run `cctv-overlay daemon` in a terminal (or under systemd) to see logs.

### States

`hidden → connecting → live`; stream errors or stalls go `reconnecting` (retries after 0.5, 1, 2, 4, 8 s,
at most 5), then `error`. `hide` always returns to `hidden`. If the stream cannot even be started
(for example no password configured) the state is `error` immediately, without retries.

### Exit codes

| Code | Meaning |
|---|---|
| 0 | Success |
| 1 | The command failed (unknown camera, invalid reload, second daemon, ...) |
| 2 | Usage or configuration error |
| 3 | Daemon unavailable (not running and could not be started, or `--no-spawn`) |
| 4 | Timed out waiting for the daemon (2 s) |

## Configuration

`$XDG_CONFIG_HOME/cctv-overlay/config.ini` (default `~/.config/cctv-overlay/config.ini`); `--config`
overrides. See `examples/config.example.ini`. Values are strictly validated and errors name the file,
section and key. Notes:

- Comments are full lines starting with `#`. A trailing `; comment` becomes part of the value and is rejected.
- Camera order is the order of `[camera.<id>]` sections; names must be unique (case-insensitive).
- `channel` ≥ 1 is required unless the camera has an `rtsp://` `url` override.
- `[recorder] host` has no default; it is required unless every camera has a `url`.
- Unknown sections/keys are errors (catches typos). A `password` key in `config.ini` is an error on purpose.

`secrets.ini` sits next to `config.ini` (`[recorder]` with `password`, optional `username`). Values are
taken literally (no quoting/escapes). Environment variables `CCTV_OVERLAY_PASSWORD` and
`CCTV_OVERLAY_USERNAME` override the file. A missing password is **not** a startup error: the daemon,
`list` and `status` work, and the error appears only when a stream is about to start.

## Security model

- Credentials live only in `secrets.ini` (keep it mode 0600; a warning names the file and mode if group/other can read it) or the environment. They are never on the command line.
- The RTSP URL, with percent-encoded userinfo, is built only at connect time in a heap buffer that is wiped after use. It is not stored in configuration structures, status output, logs or error messages.
- All log output and error replies pass through a redactor: `scheme://user:pass@` → `scheme://***:***@`, and the registered password (raw and percent-encoded) → `***`.
- The control socket is `$XDG_RUNTIME_DIR/cctv-overlay/control.sock` (directory 0700, socket 0600), restricted to the same UID via `SO_PEERCRED`. Requests are one JSON line, at most 4 KiB, with a 2 s timeout. There is no network listener.
- One daemon per user (`flock` on `daemon.lock`). The socket is removed on `quit`, SIGTERM and SIGINT.
- Installing (`make install`) only writes the binary and example files under `$(PREFIX)`; it never creates or overwrites anything in your home configuration directory.
- Known limit: the password is held in process memory while the daemon runs (and in GLib's parser buffers briefly); core dumps may therefore contain it. Consider `ulimit -c 0`.

## Test knobs (stub video, for development)

`CCTV_OVERLAY_STUB_FAIL=1` (or `first:N`), `CCTV_OVERLAY_STUB_DELAY_MS`, `CCTV_OVERLAY_STUB_DROP_AFTER_MS`,
`CCTV_OVERLAY_BACKOFF_SCALE`.
