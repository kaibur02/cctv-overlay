# Handoff — CCTV Overlay 0.1.0

## Context

This repository is a Linux (Wayland/Hyprland) picture-in-picture overlay for Dahua/RTSP cameras:
a user-level daemon (`cctv-overlay`) with a thin CLI, a GTK4 + `gtk4-layer-shell` overlay, and a libmpv
render-API video engine with VA-API decode. It is validated on Hyprland 0.56.2 with a real Dahua NVR
(five channels), and against a loopback RTSP harness for repeatable tests.

Status: **implemented and working; release polish and a few manual checks remain.** Nothing is tagged yet.

## Verified current state

- Overlay layer stays above true fullscreen, maximized and tiled windows; no keyboard focus change; typing
  reaches the application underneath; an empty input region delivered no pointer events in a probe.
- Real camera: five channels live, main stream opens in about 0.8 s, camera switch about 0.8 s; hidden means no
  player and no RTSP connection.
- 25 show/hide cycles and a 6-minute live run: stable descriptors, threads and memory; clean exit.
- `make check` (headless unit tests + 83 IPC checks) and `make check-real` (27 live loopback checks) pass.
- Arch package builds with `makepkg` (build-only test); `systemd-analyze verify` passes.
- Security/lifecycle reviews are complete; their high-value findings are fixed and covered by regression tests.
  Remaining items are documented in `ARCHITECTURE.md` §13 and the two review reports.

## What remains

1. **Licence** — MIT is proposed; add `LICENSE`, update `README.md` and `packaging/PKGBUILD`, then tag `v0.1.0`.
2. **Physical click-through** — one real click over the overlay must reach the window below. The pointer-event
   probe passed; this is the only input path not confirmed by hand.
3. **Monitor unplug while visible** — the recovery path is unit-tested; confirm on hardware.
4. **Fullscreen game / direct scanout** — a mapped overlay disables direct scanout on that monitor by design.
   Measure the frame-rate cost with the overlay shown and hidden.
5. **Live systemd user unit** — run the packaged unit in a real session, confirm start, toggle and stop.
6. **Optional hardening** — command coalescing, stricter error classification, SIGHUP, limiting the developer
   MPV option environment, and avoiding credentials in `url =` overrides. Details and priorities are in the
   review reports' Resolution sections.
7. **Publish** — repository, release notes and package.

## How to verify

```sh
make clean && make && make check
make check-real                 # live Wayland session; shows the overlay briefly on one monitor
./cctv-overlay --version
```

Read `.agent-work/HANDOFF.md` for the detailed operational handoff, including exact backup paths, test traps,
commands and remaining tasks. `ARCHITECTURE.md` §13 lists resolved and deliberately deferred review findings.
`CHANGELOG.md` records what changed and what is still open.

## Prompt for the next session

> Resume the CCTV overlay project in `/mnt/data/Projects/PCCAMOVERLAY`. Read
> `.agent-work/HANDOFF.md`, `STATUS.md`, `TASKS.md` and the two review reports first. The app is working and
> verified with a real Dahua recorder; do not redo completed work. Continue with the remaining items in the
> handoff: licence decision, physical click test, monitor unplug, game scanout measurement, live systemd unit,
> then optional deferred fixes and release. Preserve the verified state; run `make check` and `make check-real`
> after any code change. Never read the owner's `secrets.ini`; never type into the owner's opencode session
> (focus follows the mouse).
