# Changelog

All notable changes are listed here. The project follows [Semantic Versioning](https://semver.org/) once
version 0.1.0 is tagged. Nothing has been tagged yet.

## [Unreleased]

### Added

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

### Known issues and not yet done

- The real Dahua camera has not been tested. Real camera validation is pending.
- Settings accepted but not implemented yet: `start_hidden`, `preview_snapshot`, `preload_next`, `opacity`.
- Performance cost of the overlay for fullscreen games (direct scanout) has not been measured.
- Only Hyprland has been tested as a compositor.
- The `--version` string still reads `0.1.0-m2`, a leftover from the headless milestone.
- Licence not yet decided.
