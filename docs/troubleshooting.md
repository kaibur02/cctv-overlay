# Troubleshooting

Start with the state and the logs:

```sh
cctv-overlay status --json      # state, camera, last error (never credentials)
cctv-overlay ping               # is the daemon answering?
journalctl --user -u cctv-overlay.service -n 100   # if you use the systemd user unit
```

For a foreground daemon, the log is on its terminal (stderr). Increase detail with `log_level = debug` in
`[general]` of `config.ini`, then `cctv-overlay reload`. Set it back to `info` afterwards.

Exit codes: `0` ok · `1` command failed (also: a daemon is already running, or the overlay could not start) ·
`2` usage or configuration error · `3` daemon unavailable · `4` timed out waiting for the daemon.

## Daemon exits with `overlay unavailable: ...`

The real build refuses to start rather than draw an ordinary window. The message after the colon says why.

- **No Wayland display.** `echo "$WAYLAND_DISPLAY"` must print a socket name such as `wayland-1`. Run the
  daemon from your Wayland session. For the systemd unit, the user manager needs `WAYLAND_DISPLAY`
  (see [install guide](install.md#optional-systemd-user-unit)).
- **Compositor without wlr layer-shell.** GNOME (Mutter) and X11 do not support the layer-shell protocol that
  this overlay needs. The project supports wlroots-based compositors, KDE Wayland and Hyprland (per the
  gtk4-layer-shell documentation; Hyprland 0.56.2 is the version tested).
- **Link order.** `gtk4-layer-shell` must be loaded before libwayland. The Makefile already does this. If you
  changed the link line or built with other flags, check:

  ```sh
  readelf -d ./cctv-overlay | grep NEEDED | head -1
  ```

  The first `NEEDED` entry must be `libgtk4-layer-shell.so.0`. If it is not, rebuild with the Makefile.
  Without it GTK silently makes a normal window, so the daemon refuses to start instead.

## Overlay is blank (black, or no picture)

Check the state first. `status --json` should reach `live`. If it stays `connecting` for a long time, see the
connection errors below. If it is `live` and the picture is still black:

1. Hardware decode: try `hwdec = no` in `[stream]` (software decode), then `reload`. If that works, the
   hardware path is the problem. Check `vainfo` (the VA-API driver must support the codec of the stream), and
   try `hwdec = vaapi-copy`.
2. GPU or GL: run the daemon in a terminal with `log_level = debug` and look for `decoder`, `first frame` and
   GL or EGL errors. Verify that the GPU driver works in the session (other GL apps render).
3. A camera key-frame wait is normal: the first picture waits for the next key frame of the sub stream, which
   can take up to a GOP length (a few seconds on some cameras). This is not a fault.

## Wrong password: `authentication failed (check username and password)`

- Check `secrets.ini`: `password` (and `username` if your recorder account is not the default). Values are
  literal. Remove stray quotes and trailing spaces.
- Check that the file is the one the daemon reads (next to `config.ini`, or the environment variables
  `CCTV_OVERLAY_PASSWORD` / `CCTV_OVERLAY_USERNAME`, which override the file).
- The state goes to `reconnecting` and then `error`. Run `cctv-overlay reload` after editing `secrets.ini`,
  then `cctv-overlay show`.
- Keep the password out of command lines and shell history. cctv-overlay has no option that takes it.

## `connection refused (is the recorder address and port right?)`

- `[recorder] host` must be the recorder's LAN address (for example `192.168.1.100`), and `port` must be the
  RTSP port (default `554`). To test the port from the same machine, use a connection test such as
  `nc -vz 192.168.1.100 554` if netcat is installed.
- The recorder must have RTSP enabled and the port must match its network settings. The recorder's web UI can
  be reachable even when RTSP is off, so a working web page does not prove RTSP works.
- Do not change firewall rules to fix this unless you know what you are opening. The overlay makes outbound
  TCP connections only; it has no network listener.

## `could not open the stream (unreachable, refused or timed out)`

Network path or recorder problem: the host does not answer, the route is wrong, or the stream does not start
within about five seconds. Check that the recorder is powered and on the same network. A stream that stalls
after a live start is reported as `reconnecting`, then `error`, and retried with backoff.

## Overlay appears under fullscreen windows, or not at all

- The overlay must use layer `overlay`, which is the default. The `top` layer is hidden under fullscreen windows
  on Hyprland 0.56.2. Keep `layer = overlay`.
- Confirm the layer is there. The entry should appear at level 3 under the monitor you chose:

  ```sh
  hyprctl layers | grep -B2 -A3 cctv-overlay
  ```

  If nothing shows, the daemon is not visible (check `status`) or the monitor is not the one you expect. Set
  `monitor = DP-1` (your connector) to rule out `focused`.
- A fullscreen game or video can still look different: a visible overlay turns off direct scanout on that
  monitor, so performance may change. This is expected (see [limitations](../README.md#limitations)).

## Clicks or typing go to the overlay instead of the window below

The overlay has an empty input region, so pointer events should go to the window underneath, and keyboard mode
is `none` so it should never take focus. If that is not the case:

- Check `hyprctl activewindow`: the focused window should not change when the overlay is shown.
- Check that you are not running an old build. Rebuild and reinstall.
- Report it with the Hyprland version and the output of `hyprctl layers`.

## Focus follows the mouse

If Hyprland's `follow_mouse` is enabled, moving the pointer over the overlay can change the focused window
to the window under it, as it would for any window. This is normal and it is not focus stealing by the overlay.
Keyboard input goes to whichever window is focused, so do not type into a fullscreen window while the pointer
crosses the overlay without checking the focus.

## `cctv-overlay` says a daemon is already running (exit 1)

Only one daemon per user is allowed (lock file `daemon.lock` next to the socket). Find it:

```sh
pgrep -a -f "cctv-overlay daemon"
```

Use `cctv-overlay quit` to stop it, or `systemctl --user stop cctv-overlay.service`.

## `daemon unavailable` (exit 3) or `timed out` (exit 4)

- The socket is `$XDG_RUNTIME_DIR/cctv-overlay/control.sock`. Check it with `ls -l "$XDG_RUNTIME_DIR/cctv-overlay/"`.
  The socket is mode `0600` and the directory is `0700`, owned by you.
- If you run the daemon with `--socket PATH`, every client needs the same `--socket PATH`.
- A client started with `--no-spawn` never starts the daemon. Start it yourself, or drop `--no-spawn`.
- An auto-started daemon has no terminal. Check the logs with the systemd unit, or start it in a terminal.

## Configuration errors

Errors name the file, section and key. Common ones:

- `config file not found`: create `~/.config/cctv-overlay/config.ini` from the template. The installed template is
  `/usr/share/doc/cctv-overlay/config.example.ini` (package, or `make install` with the default prefix). The
  message itself still names the source-tree path `examples/config.example.ini`; that is a known wording issue.
- A trailing comment after a value (`width = 480  # pixels`) becomes part of the value and is rejected. Comments
  must be full lines starting with `#`.
- `password` in `config.ini` is rejected on purpose. Move it to `secrets.ini`.
- An unknown key or section is rejected, so check for typos.
- A `secrets.ini` readable by other users triggers a warning. Fix it with `chmod 600 ~/.config/cctv-overlay/secrets.ini`.

## Camera switches to the wrong picture

`channel` is the Dahua channel number, starting at 1. Confirm it on the recorder's web UI. Cameras are cycled
in the order of the `[camera.*]` sections in `config.ini`. `cctv-overlay list` shows that order.

## Sharing logs

Logs redact the password and the RTSP credentials (`rtsp://***:***@`), but redaction is a safety net, not a
guarantee. Before you share a log or `status` output in a bug report:

- Search it for passwords, the recorder address, and camera names you consider private:

  ```sh
  grep -iE 'password|passwd|secret|rtsp://' logfile.txt
  ```

- Remove any other personal details. Never share `secrets.ini`.
- Do not share a core dump. It can contain the password (see [behaviour](behaviour.md#security-model)).

## Diagnosing slow start-up (developer switches)

Two environment variables exist for diagnosis only; they are not part of the stable interface:

- `CCTV_OVERLAY_MPV_TRACE=1` (or `=trace`) makes the daemon forward verbose libmpv/FFmpeg log lines, each prefixed with
  `mpvtrace +<ms>`. They pass through the same redactor as every other log line (`rtsp://user:pass@` becomes
  `rtsp://***:***@`), but at `trace` level FFmpeg also logs RTSP authentication headers. **Never share a trace log
  without reading it first.** With it you can see when the recorder answered `DESCRIBE`, when `PLAY` was sent and when the
  first keyframe arrived.
- `CCTV_OVERLAY_MPV_OPTS="name=value;name=value"` sets extra libmpv options for A/B experiments, applied after the
  defaults.
