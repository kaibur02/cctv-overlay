# CCTV Overlay for Linux

A lightweight, self-contained Linux application that shows a live Dahua/RTSP camera feed in a floating,
click-through picture-in-picture overlay above the desktop — including fullscreen applications where the
compositor permits it.

> **Status: planning / feasibility (Milestone 0 complete). No application code yet.**
> The implementation gate is pending the owner's go-ahead. See [`PLAN.md`](PLAN.md),
> [`ARCHITECTURE.md`](ARCHITECTURE.md) and [`DELEGATION.md`](DELEGATION.md).

## What it will do

- **Toggle** the overlay with one configurable action (keyboard shortcut assigned later by the user).
- **Cycle** cameras with a separate configurable action, wrapping around the configured list.
- Show video **above normal windows and fullscreen content** using the Wayland layer-shell overlay layer.
- **Never take keyboard focus** and **pass mouse clicks through** to the application underneath.
- Decode the NVR's **sub-stream** with VA-API hardware decoding and a low-latency RTSP profile.
- Release the player, network connection and GPU resources when the overlay is hidden.
- Keep credentials **out of source, Git, logs and process listings**.
- Run as a small user-level daemon with a Unix-socket control interface, so any desktop can bind its own keys.

## Planned technology

| Layer | Choice | Why |
|---|---|---|
| Overlay surface | GTK4 + `gtk4-layer-shell` (`wlr-layer-shell`, `overlay` layer) | The only reliable Wayland way to sit above fullscreen content without taking focus or input |
| Video | `libmpv` render API into a `GtkGLArea`, VA-API decode | Already installed, mature RTSP/HEVC handling, low-latency profile, reusable GPU surfaces |
| Control | Unix domain socket + JSON lines, thin CLI | No keyboard handling in the app; any compositor or script can call it; no network exposure |
| Config | INI via GLib `GKeyFile`, separate `secrets.ini` (0600) | No extra parser dependency, comments, ordered camera groups |
| Build | C11 + `make` + `pkg-config` | Small dependency set; all build headers already present on the target system |

These choices are **provisional** until the Milestone 1 spike validates fullscreen visibility, input
passthrough and libmpv/GTK4 rendering. Alternatives and rejection reasons are in
[`ARCHITECTURE.md`](ARCHITECTURE.md).

## Target environment (verified 2026-11-08)

- Omarchy 4.5.0 / Arch Linux, Hyprland 0.56.2 on Wayland (Aquamarine 0.15.1)
- AMD Ryzen 5 5600X, Radeon RX 9070 XT (Navi 48, `gfx1201`), Mesa 26.2.5, kernel 7.2.15
- VA-API H.264/HEVC/AV1 decode confirmed by `vainfo`
- Dual monitor: DP-2 1920x1080 @ 99.93 Hz at (0,0); DP-1 2560x1440 @ 164.96 Hz at (1920,-110), scale 1
- Recorder: Dahua device at the owner's private LAN address — RTSP/554 open, Dahua-style Digest auth, web
  UI reachable (the address in the original brief resolved to a printer, not the recorder; the owner
  corrected it). The concrete local address lives only in the owner's config and internal project memory.

## Security

The repository contains **no credentials**. The public example config has no password field populated.
Local credentials live in `~/.config/cctv-overlay/secrets.ini` with mode `0600` (or an environment
variable), are never written to logs, and are never passed on a command line. See `ARCHITECTURE.md`
§ Security model.

## Documentation

- [`PLAN.md`](PLAN.md) — milestones, acceptance mapping, risks and performance targets
- [`ARCHITECTURE.md`](ARCHITECTURE.md) — components, protocols, config schema, open technical questions
- [`DELEGATION.md`](DELEGATION.md) — how the work is split across agents and waves
- [`docs/adr/`](docs/adr/) — public architecture decision records
- `.agent-work/` — internal project memory and specialist reports (not part of a public release)

## Licence

TBD — MIT is the proposed default; the owner decides before the first public release.
