# Changelog

All notable changes are listed here. The project follows [Semantic Versioning](https://semver.org/) once
version 0.1.0 is tagged. Nothing has been tagged yet.

## [Unreleased]

### Added

- **Grid view**: up to four cameras in one 2×2 frame (`[grid]`, default the first four). It is the first view on
  every `toggle`; `next`/`prev` cycle grid → camera 1 → … → camera N → grid, wrapping both ways. New `grid`
  command. Each tile has its own libmpv player (zero-copy VA-API), state and reconnect backoff.
- **Audio**: `cctv-overlay audio [on|off]` toggles camera sound live (single-camera view only); a drawn speaker
  icon shows in the overlay corner (crossed out when the stream has no audio track, dimmed in the grid).
  `[stream] audio` sets the start value. With audio off no audio output is opened.
- **Other camera systems**: `[recorder] path` templates (`{channel}`, `{channel02}`, `{subtype}`, `{stream}`,
  `{mainsub}`) and per-camera `host`/`port`/`path` for Hikvision, Reolink, Uniview, Tapo and separate IP cameras.
  Credentials are still inserted only from `secrets.ini`.
- `cctv-overlay check`: offline validation of config and secrets (never prints secrets).
- `install.sh`: guided installer (requirements, build, install, camera system, cameras, monitor, size, key
  bindings in a managed block with backup and automatic rollback, hidden password prompt, live test), with
  `--answers` for unattended/AI installs, `--set-password`, `--check`, `--test`, `--uninstall`.
- Docs: `docs/requirements.md`, `docs/ai-install.md`, `examples/install.answers.example`.
- Status JSON: `view`, `audio`, `tiles`; `list --json` marks `in_grid`.

- Daemon `cctv-overlay daemon` with a thin CLI client: `toggle`, `next`, `prev`, `show`, `hide`, `status`
  (`--json`), `list`, `reload`, `quit`, `ping`.
- Real video engine: libmpv render API in a GTK 4 `GtkGLArea`, VA-API hardware decode, low-latency RTSP profile,
  RTSP over TCP by default, fresh player per session, released when hidden.
- Overlay window: gtk4-layer-shell surface in the `overlay` layer, empty input region (click-through), keyboard
  mode `none`, configurable size, anchor, margins and monitor. Startup fails loudly (`overlay unavailable`)
  instead of drawing an ordinary window.
- Two-file configuration: `config.ini` (no secrets) and `secrets.ini` (mode `0600`), with environment
  variable overrides. Strict validation; unknown keys are errors.
- Credential redaction for logs and error replies; no credentials in command lines or status output.
- Unix-socket control with `SO_PEERCRED` checks and a per-user daemon lock.
- Reconnect with backoff after stream errors and stalls.
- `make install` / `make uninstall` (binary and examples only; never touches the user's configuration).
- Draft Arch `PKGBUILD` (`packaging/`), hardened systemd user unit (`packaging/cctv-overlay.service`, not enabled
  by default).
- Optional Hyprland example snippets for Lua (Hyprland 0.55+) and classic hyprlang (`integrations/hyprland/`),
  using placeholder keys.
- Documentation: install, configuration, Hyprland integration, troubleshooting, behaviour, overlay.
- Opt-in test suite: `make check` (headless) and `make check-real` (live Wayland session, loopback RTSP).

### Verified

- Hyprland 0.56.2: overlay visible above a true fullscreen window, no focus change, typing reaches the
  fullscreen application, empty input region delivered no pointer events in a probe (physical click test pending).
- Loopback RTSP test harness: live playback with Digest authentication, percent-encoded passwords, next/prev/show
  switching, failure paths (wrong password, refused connection), and no credentials in daemon output.

### Verified with the owner's Dahua recorder

- Five real channels (1-5) live in the overlay, above a true fullscreen window, with no focus change and no
  credentials in logs, `ps`, `/proc/PID/cmdline` or status output.
- Measured toggle to first picture: **about 0.8 s on the main stream**, about 2.7 s on the sub stream (the
  recorder starts sub streams on demand). Camera switches about 0.8 s; cold daemon start + toggle 0.91 s.
  While visible: about 2% of one CPU core, 3 Mbit/s, ~230 MB RSS; nothing runs while hidden.
- 25 show/hide cycles plus a 6-minute continuous run: no failures, flat descriptors/threads/memory, clean exit.

### Fixed after the 2026-11-08 reviews

- Window loss while visible no longer leaves the app stuck; `toggle`/`show`/`next` re-create the overlay (F1).
- A stream that delivers one frame and drops now exhausts the retry budget instead of looping forever (F2).
- `reload` in `error`/`reconnecting` now retries the stream (F9).
- CONNECTING has a timeout that does not depend on render callbacks; `MPV_EVENT_SHUTDOWN` is handled (F5/F6).
- Redaction: invalid UTF-8 can no longer bypass the URL rule; `Authorization:` headers and Basic base64
  credentials are removed; credentials in `url =` overrides are registered; trace mode requires exactly `1` or
  `trace` (S1/S2/S3). Control characters are stripped from log lines (S7).
- The daemon sets `RLIMIT_CORE=0`; the auto-spawn log is opened with `O_NOFOLLOW` and forced to 0600 (S4/S6).
- `--socket` refuses to remove a path that is not a socket (S8/F4); concurrent control connections are capped
  at 8 (S9); test backoff scaling is clamped (S12).
- Build uses `-D_FORTIFY_SOURCE=2 -fstack-protector-strong`; test suite now covers the new failure paths.

### Known issues and not yet done

- Rapid camera commands are not coalesced (F3); hundreds per second can block the UI.
- Error classification still uses substring matching (F7).
- SIGHUP is not handled (F10); the stale-socket recovery covers the normal consequence.
- `CCTV_OVERLAY_MPV_OPTS` can set arbitrary libmpv options (F11, developer-only, same-UID).
- Credentials in a `url =` override are registered but still stored in the config object (F12).
- Plain RTSP is unencrypted; no `rtsps://` support (S5). Secrets file mode is advisory (S11).
- The systemd unit has not been run live (S13).
- Physical click-through, monitor unplug, and the frame-rate cost of losing direct scanout in a game have not
  been tested by hand.
- Only Hyprland has been tested.
- Licence not yet decided.
