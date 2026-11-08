# Implementation plan — Linux CCTV Picture-in-Picture Overlay

**Version:** 0.1 planning
**Date:** 2026-11-08
**Owner:** project lead (Tier 1)
**Status:** M0-M4 complete and validated; M5-M7 drafted. Remaining work is release polish, the deferred review items listed in §13 of ARCHITECTURE.md, and the real-camera/game/click checks in the handoff.

## 1. Objective

Deliver a maintainable, publicly distributable Linux application that shows a live Dahua/RTSP camera
feed in a small floating overlay above ordinary and fullscreen desktop content, with near-instant
opening and camera switching, minimal resource use, no input interference, and no credential exposure.

A separate configurable action toggles the overlay; another cycles cameras. Keyboard assignments are
deliberately deferred to the owner and are not hardcoded anywhere.

## 2. Verified starting point (2026-11-08)

| Fact | Evidence |
|---|---|
| Omarchy 4.5.0 (Arch), Hyprland 0.56.2, Wayland, Aquamarine 0.15.1 | `/etc/os-release`, `hyprctl version` |
| Hyprland user config is Lua (Omarchy 4.5 style) | `~/.config/hypr/*.lua`, `hl.exec_cmd(...)` in Omarchy defaults |
| AMD RX 9070 XT, Mesa 26.2.5, kernel 7.2.15 | `lspci`, `vainfo` |
| VA-API decode: H.264, HEVC, VP9, AV1 | `vainfo` |
| GTK4 4.24.1, gtk4-layer-shell 1.3.0 (headers + `.pc`), libmpv 2.5.0 (render API headers) | `pkg-config`, `pacman -Ql` |
| libcurl 8.23.0 dev, json-glib 1.10.8, libyaml 0.2.5 | `pkg-config` |
| No cmake/meson/ninja; gcc 16, make, pkg-config present | `command -v` |
| GStreamer 1.28.8 present but **no** `gtk4paintablesink`, **no** `vah264dec`/`vah265dec` | `gst-inspect-1.0` |
| Recorder at the owner's LAN address: RTSP/554 open; HTTP 200 Dahua-style "WEB SERVICE"; Digest realm matches RTSP 401 | `curl`, raw RTSP `OPTIONS` |
| The brief's original address was an Epson printer (`SERVER: EPSON_Linux ...`), no RTSP | `curl -I`, port probe |
| Monitors: DP-1 2560x1440@165 at (1920,-110); DP-2 1920x1080@100 at (0,0); scale 1 | `hyprctl monitors -j` |
| Existing desktop tools useful for tests: `grim`, `wtype`, `slurp`, `magick`, Python Pillow, `ffmpeg`, `steam`, `gamescope` | `command -v` |
| `wf-recorder`, `nmap`, `radeontop`, `ydotool` absent | `command -v` |

Credentials were **not** supplied at planning time. RTSP reachability and the Dahua Digest challenge are
verified; stream contents, channel map and sub-stream parameters are not yet verified.

## 3. Acceptance criteria and evidence plan

| # | Requirement | Planned evidence |
|---|---|---|
| A1 | Connects to the Dahua recorder configured by the owner | `cctv-overlay probe` output (resolution, codec, fps) for every configured camera; packet capture summary if needed |
| A2 | Live playback, no recording/relay | Live timestamp/watermark visible; `ss` shows a single direct RTSP/TCP session to the recorder's port 554 |
| A3 | Overlay above normal desktop content | Screenshot (`grim`) with overlay over a tiled and a floating window; pixel check of a test pattern |
| A4 | Visible over fullscreen where supported | Screenshot matrix: native fullscreen mpv, fullscreen Chromium, fullscreen game/Proton, gamescope, Hyprland fullscreen modes 1/2/3; documented per case |
| A5 | Keyboard/mouse passthrough | `hyprctl activewindow` unchanged while overlay shown; `wtype` types into the focused fullscreen app; manual click-through check on a button under the overlay |
| A6 | One configurable action toggles | `cctv-overlay toggle` from CLI and a test Hyprland bind; state transitions in `status --json` |
| A7 | Separate action cycles cameras | `cctv-overlay next` / `prev` with wrap-around; logs and status show the selected camera |
| A8 | Minimal switching delay | Instrumented timestamps (command→preview, command→first live frame) over 20 switches; target table in §7 |
| A9 | Efficient resource use | `VmRSS`, CPU %, thread count, FD count, `gpu_busy_percent` sampled visible vs hidden; target table in §7 |
| A10 | Clean lifecycle | After 20 toggle cycles: RTSP sockets closed, threads and FDs return to baseline, RSS returns near baseline; `quit` leaves no process/socket |
| A11 | Configurable without source changes | All camera/recorder/overlay fields in config; a second test config with different host/channel works unmodified |
| A12 | Credentials not exposed | `grep` audit of Git history/logs/argv; redaction unit tests; `ss -p`/`ps` show no password; secrets file 0600 |
| A13 | Portable core | Test with a non-Omarchy Hyprland config (no Lua assumptions), Sway if available; distro dependencies documented |
| A14 | Git-contained project | Single repository, clean tree, tagged release, install/uninstall documented |
| A15 | Documented installation | Fresh-clone build + `make install`/PKGBUILD instructions; example config copied and populated |

Not achievable if the compositor hides layer surfaces in fullscreen (A4): the limitation will be documented
with the exact compositor/version and a fallback, never claimed as working.

## 4. Milestones

### M0 — Planning and environment reconnaissance (complete)
Deliverables: this plan, architecture, delegation plan, `.agent-work/` state, verified environment facts,
camera endpoint identification. **Exit:** plan reviewed; no code written.

### M1 — Feasibility spike (critical gate)
Prove or disprove the three risky assumptions **before** building the product:

1. A `wlr-layer-shell` overlay surface is visible above Hyprland fullscreen content (all fullscreen modes,
   native Wayland app, gamescope, Proton game) and does not steal keyboard focus.
2. An empty input region gives real mouse click-through while the overlay stays visible.
3. `libmpv` render API + `GtkGLArea` + VA-API produces a stable, low-latency frame path on this GPU.

Deliverables: `spike/` sources, executed test matrix with screenshots/logs, measured cold-start and
switch times, and ADR-0001/0002 marked Accepted or superseded.

**Exit (GO):** A3/A4/A5 demonstrated on Hyprland for at least one fullscreen case, with a documented
matrix and a credible path for the rest. **NO-GO:** document the compositor limitation and propose the
best fallback (borderless window + compositor rules) before any product code.

### M2 — Headless core
Config/secrets loading and validation, camera model, ordered camera list, state machine, Unix-socket IPC,
CLI (`toggle`, `next`, `prev`, `show`, `hide`, `status`, `list`, `reload`, `quit`), logging with
redaction, single-instance handling, signal handling.

**Exit:** unit tests pass; `cctv-overlay status --json` works with no video; malformed configs produce
actionable errors; `grep` of logs shows no password.

### M3 — Basic playback (ordinary window)
libmpv engine, URL construction with percent-encoding, hardware decode, low-latency options, first-frame
and error events, RTSP transport A/B, real camera test (credentials required), local RTSP harness for
repeatable tests. A plain GTK window is acceptable here; no overlay work yet.

**Exit:** live feed in a normal window; measured startup; A1/A2; no audio playing; clean stop.

### M4 — Floating overlay
Layer-shell integration, `overlay` layer, anchors/margins/size from config, monitor targeting,
keyboard-interactivity NONE, empty input region, no decorations, label/status indicator, optional
snapshot preview, rounded-corner treatment if it costs little.

**Exit:** A3/A4/A5 on Hyprland, evidenced by screenshots and focus/typing checks. The fullscreen matrix is
recorded per compositor/version, including failures.

### M5 — Controls and switching
Toggle/show/hide lifecycle, `next`/`prev` with wrap, cycle-while-closed behaviour, snapshot-first switch,
optional next-camera preload, timing instrumentation exposed through `status`.

**Exit:** A6/A7/A8; 20-switch run with p50/p95 timings; documented behaviour when closed.

### M6 — Reliability and performance
Reconnect/backoff, stall watchdog, error states, release-on-hide, 1-hour soak, 20-toggle leak test,
FD/thread/RSS/GPU sampling, multi-monitor and workspace tests, compositor portability checks.

**Exit:** A9/A10; no leaks; reconnect demonstrated by stopping/starting the local RTSP harness; failures
produce a visible state, not a hang.

### M7 — Packaging, documentation and release readiness
Makefile install/uninstall, PKGBUILD, optional systemd user unit, example configs, Hyprland integration
snippets (Lua and classic), README/install/troubleshooting, licence, `CHANGELOG.md`, `v0.1.0` tag.
Installation must never overwrite an existing user config or secrets file; it installs examples only and
prints the exact copy command. Independent security review and adversarial code review completed; findings
fixed or documented.

**Exit:** A11–A15; a fresh clone can be built, configured and run following only the docs.

## 5. Workstreams and sequence

| WS | Scope | Primary agent | Depends on |
|---|---|---|---|
| WS1 | Compositor/overlay feasibility and implementation | research-deep → lead spike → implementer | — |
| WS2 | Video engine (libmpv, VA-API, latency) | research-budget → implementer | M1 |
| WS3 | Core: config, IPC, CLI, state machine | implementer | M1 decisions |
| WS4 | Config UX, security, packaging, docs | implementer + security-reviewer | WS3 API |
| WS5 | Independent verification (QA matrix, soak, latency) | tester | M2–M6 |
| WS6 | Adversarial review of critical paths | reviewer-premium | M5/M6 |

M2 and WS2 research can overlap. WS1's spike is deliberately first because a failure there changes the
whole design.

## 6. Risk register

| ID | Risk | Likelihood | Impact | Mitigation / early test |
|---|---|---|---|---|
| R1 | Hyprland hides layer surfaces behind fullscreen windows | Medium | Critical | M1 spike, screenshot pixel check; if true, document limitation and test a borderless-window fallback; consider an upstream issue, not a private plugin |
| R2 | libmpv render API + GTK4 GL context unstable or wrong colour/flip | Medium | High | Minimal C spike; fallback to GStreamer + `gtk4paintablesink` (requires package install and approval) or `vaapi-copy` |
| R3 | Credentials/channel map unknown | High until supplied | High | User populates `secrets.ini`; `probe` reports codec/resolution/fps per channel; never assume channels exist |
| R4 | Click-through does not work as expected | Medium | High | Empty input region + keyboard mode NONE; verify focus via `hyprctl`, typing via `wtype`, manual mouse test; document if a compositor needs a rule |
| R5 | Startup latency dominated by camera GOP/IDR | High | Medium | Sub-stream first, snapshot preview, `low-latency` profile, TCP/UDP A/B, optional preload; measure, don't assume |
| R6 | Overlay forces composition and costs game performance / breaks VRR | Medium | Medium | Measure fps/GPU with overlay on/off in a fullscreen game; document; offer "hide during games" only if needed |
| R7 | Multi-monitor placement with negative origins / scale | Low | Medium | Use GDK monitor geometry; test both outputs at scale 1 and a simulated scale 1.25 |
| R8 | Credential leak via logs, argv, core dump or error strings | Low | Critical | No credentials in argv; redaction layer + tests; no mpv URL logging; secrets 0600; security review; `G_DEBUG`/core policy documented |
| R9 | Dependency/install friction on other distros | Medium | Medium | Arch PKGBUILD first; document package names; keep libmpv/GTK4/layer-shell only |
| R10 | Hyprland Lua vs classic config drift | Low | Low | Provide both snippets; app itself never edits compositor config |
| R11 | RTSP over UDP loses packets; TCP adds latency | Medium | Medium | Default TCP (stability), configurable; measure both on wired LAN |
| R12 | NVR connection limit with preload | Low | Medium | Preload off by default; probe documentation; release spare on hide |

## 7. Targets (not yet verified)

| Metric | Target |
|---|---|
| Hidden idle CPU | 0% (no decode, no polling) |
| Hidden network connections | 0 |
| Daemon hidden RSS | ≤ 80 MB |
| Visible CPU (704×576 HEVC sub-stream) | ≤ 5% of one core |
| Visible RSS | ≤ 180 MB |
| Toggle → preview frame | ≤ 250 ms |
| Toggle → first live frame (LAN) | ≤ 1.2 s p95 |
| Switch → preview frame | ≤ 150 ms |
| Switch → first live frame | ≤ 900 ms p95 |
| Frame drops at 12 fps | < 1% over 10 min |
| 20 toggles | no FD/thread growth; RSS within +5 MB of baseline |
| Clean quit | socket removed, RTSP sockets closed, process gone |

The latency floor is the camera's IDR interval. If p95 misses the target, that is a camera-configuration
fact to document (e.g. reduce GOP on the sub-stream), not something to hide.

## 8. Test strategy

1. **Unit tests (C):** config parsing/validation, ordered cameras, URL percent-encoding (passwords with
   `@ : / %`), log redaction, IPC framing, state transitions.
2. **Local integration harness:** `ffmpeg` RTSP listener with a generated H.264/H.265 test pattern
   (`-rtsp_flags listen`), looped; start/stop to exercise reconnect; no external downloads.
3. **Real camera:** `probe` and playback with the owner's credentials; record codec/resolution/fps/GOP
   if available; test each configured channel; never log the URL.
4. **Compositor matrix:** Hyprland (primary), Sway if installable, KDE if available; document results
   per version. Fullscreen cases: native Wayland mpv, Chromium, Proton game, gamescope, Hyprland modes
   1/2/3.
5. **Input tests:** focus unchanged, `wtype` into underlying app, manual mouse click-through.
6. **Performance:** instrumented timings, `ps`/`/proc`, sysfs GPU busy, 1-hour soak, 20-toggle leak run.
7. **Security:** `grep` history and logs for secrets; `ps`/`ss` inspection; file mode checks; independent
   security review.

No test is marked passed without a command, exit status and a result summary in `.agent-work/TESTS.md`.

## 9. Out of scope for v0.1

- Multi-camera grid / mosaic.
- Recording, snapshots to disk, motion alerts, PTZ control.
- Cloud relays, remote access, mobile apps.
- Audio monitoring (video-only by design).
- GUI settings editor (config file + `probe`/`status` are enough).
- Non-Dahua-specific protocol work beyond a configurable RTSP URL override.
- Automatic editing of the user's compositor configuration.

## 10. Approvals and prerequisites

| Item | Status |
|---|---|
| Credentials in `~/.config/cctv-overlay/secrets.ini` (owner-populated) | Pending |
| Confirm recorder LAN address | Confirmed by owner 2026-11-08 (recorded in `.agent-work/RESEARCH.md`) |
| Any package installation (only if a fallback needs it) | Ask before install; none required for the planned stack |
| Runtime compositor keyword changes during tests (e.g. `direct_scanout`) | Ask first; prefer tests with current settings |
| Licence choice (MIT proposed) | Pending; not blocking implementation |
| Public release / repository hosting | Later |

## 11. Definition of done (v0.1.0)

All acceptance criteria A1–A15 have evidence in `.agent-work/TESTS.md`; the fullscreen matrix is honest
about any compositor limitation; a fresh clone builds and runs from the documented steps; credentials are
absent from Git, logs and argv; the owner has used it with their own shortcuts and confirmed the feel.

## 12. Status at 2026-11-08

| Milestone | State |
|---|---|
| M0 recon | complete |
| M1 spike | complete; GO (ADR-0001/0002 accepted) |
| M2 core | complete; 83 headless checks |
| M3 playback | complete; real camera live |
| M4 overlay | complete; above true fullscreen, click-through probe passed, physical click pending |
| M5 controls | complete; toggle/next/prev/show/hide, cycle-while-closed, measured latency; snapshot preview not needed (0.8 s achieved) |
| M6 reliability | mostly complete; 25-cycle + 6-minute soak, fd/thread/RSS stable; F3/F7/F10 remain |
| M7 packaging/docs | draft complete; `makepkg` build passed; unit not run live; licence TBD |

Open release blockers: F3 (optional), licence choice, physical click check, monitor-unplug check, game scanout
measurement, systemd unit live test. Security fixes S1-S4/S6-S10/S12/S14 are applied and covered by tests.
