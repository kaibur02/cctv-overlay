# ADR-0003: Control the app over a Unix socket with a thin CLI

- **Status:** Accepted (implemented in M2; 83 headless checks, same-UID socket, CLI auto-spawn and stale-socket recovery verified)
- **Date:** 2026-11-08
- **Deciders:** project lead
- **Related:** ARCHITECTURE.md §3.5–§3.6

## Context

The owner will choose keyboard shortcuts later and may change them. The application must not hardcode
keys or depend on one compositor's configuration. It should also be scriptable for testing and future
integrations (stream deck, bar widget, hotkey daemon).

## Options considered

1. **Unix domain socket + JSON lines + CLI** — compositor-agnostic, local-only, easy to test, no daemon
   framework.
2. **D-Bus service** — standard on Linux desktops, but more boilerplate, activation semantics and
   introspection work; harder to test in a container/session.
3. **Signals only** — too limited for arguments and status.
4. **In-process key grabbing** — explicitly out of scope; would fight the compositor and the brief.

## Decision

Run a small user-level daemon owning the overlay and player. Expose commands over
`$XDG_RUNTIME_DIR/cctv-overlay/control.sock` (0700 directory, 0600 socket, same-UID peer check) using one
JSON object per line: `toggle`, `next`, `prev`, `show`, `hide`, `status`, `list`, `reload`, `quit`, `ping`.
Ship a thin CLI (`cctv-overlay <command>`) that auto-starts the daemon if needed and returns meaningful
exit codes. Provide optional systemd user unit and Hyprland snippets; never edit compositor config.

## Consequences

- Shortcuts are just `exec` bindings; no key handling in the app.
- The daemon is the single source of truth for state; the CLI is stateless.
- The socket is a local trust boundary; any same-UID process can control the app (acceptable: it can also
  read the secrets file).
- Testing can drive the real app deterministically without a keyboard.

## Revisit when

- A standard desktop integration (D-Bus, portals) is required for a public release.
- Sandboxing (Flatpak) needs a portal-based control path.
