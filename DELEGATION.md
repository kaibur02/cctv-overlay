# Delegation plan

**Principle:** the lead owns decisions, integration, the desktop-interactive acceptance tests and the central
project memory. Specialists get small, separable tasks with one question, one file scope and one report path.
No two agents edit the same file. The lead verifies every claim that matters before accepting it.

## 1. Operating rules for every task packet

- One report per task at `.agent-work/reports/<date>-<task-id>-<agent>.md`; never append to another agent's
  report and never edit `.agent-work/STATUS.md`, `TASKS.md`, `DECISIONS.md`, `RESEARCH.md`, `TESTS.md` or
  `HANDOFF.md` (lead-owned).
- Report format: objective, conclusion (`complete|partial|blocked|unverified`), findings with confidence,
  evidence/URLs, files inspected/changed, commands with exit status, tests, risks/assumptions/open questions,
  exact next action.
- **Every packet carries this line:** "Change authority: follow the global change-authority checklist.
  In-scope reversible changes need a restorable backup, then you may proceed. Actions on the dangerous list
  are never reasonable; stop and ask the user before any tool call. A tool deny beats this packet."
- No secrets in reports, logs or commands. No credentials in argv. Never print a full RTSP URL.
- No package installs, service changes or privileged commands without a user answer routed through the lead.
- Stop after two failed attempts at the same approach; change method. After three unproductive cycles,
  report a blocker with evidence. Do not escalate.

## 2. Waves

| Wave | When | Agents (profile → model) | Purpose | Gate |
|---|---|---|---|---|
| 0 | Done | lead (DeepSeek V4.1 Flash) | Recon, plan, architecture, state | User review |
| 1 | On go-ahead | `research-deep` (Kimi K3.1 Pro) + `research-budget` (MiMo V2.6 Flash) | Close the compositor and video-engine unknowns from documents/source | Evidence bundle reviewed by lead |
| 2 | After Wave 1 | `implementer-code` (Kimi K2.7 Code) | Minimal spike: layer-shell visibility/input + libmpv→GtkGLArea | Lead runs matrix; GO/NO-GO |
| 3 | After GO | `implementer` (DeepSeek V4.1 Flash) | M2 core: config, IPC, CLI, state machine | Unit tests + lead review |
| 4 | After M2 | `implementer-code` (Kimi K2.7 Code) | M3/M4 video engine + overlay integration | Lead runs camera/fullscreen tests |
| 5 | After M4 | `implementer` (DeepSeek V4.1 Flash) | M5 controls, switching, snapshot preview, timing | Lead measures latency |
| 6 | After M5 | `tester` (GLM-5.3 Flash) + lead | M6 independent QA, soak, leak and compositor matrix | Test report verified by lead |
| 7 | After M6 | `security-reviewer` (Kimi K3.1 Pro) then `reviewer-premium` (Grok 4.7) | Credential/IPC/lifecycle security review; adversarial code review | Findings fixed or documented |
| 8 | After reviews | `implementer` (DeepSeek V4.1 Flash) + lead | M7 packaging, docs, release candidate | Fresh-clone install test |

Premium calls are deliberately narrow: one compositor research task, one security review, one adversarial
review. Routine implementation stays on the default workhorse. If a model is unavailable or underperforming,
swap to the equivalent profile (e.g. `implementer-code` instead of `implementer`) and record why in
`DECISIONS.md`.

## 3. Wave 1 task packets (ready to launch, not started)

### W1-A — Compositor overlay feasibility (critical)

- **Agent:** `research-deep`
- **Objective:** answer, with upstream evidence, whether a `wlr-layer-shell` `overlay`-layer surface on
  Hyprland 0.56.2 stays visible above fullscreen windows (modes 1/2/3), how input regions and keyboard
  interactivity behave, and what happens to direct scanout. Cover Sway/KDE/GNOME support boundaries.
- **Scope:** Hyprland source/docs/issues for 0.56.x, `wlr-layer-shell` protocol, gtk4-layer-shell docs,
  Hyprland layer rules and `misc:on_focus_under_fullscreen`, fullscreen render order, direct scanout.
  Do not edit code.
- **Deliverable:** `.agent-work/reports/2026-11-08-w1a-compositor-research-deep.md` with a claim table
  (claim, source URL, version, confidence), a list of exact empirical checks for the lead to run, and the
  top three failure modes.
- **Acceptance:** every high-impact claim has a primary source (source file, protocol XML, official docs or
  maintainer statement), clearly separated from inference. Explicitly state what remains unverified.
- **Stop:** if sources conflict, present the conflict and the decisive experiment; do not pick a side
  without evidence.
- Change authority: follow the global change-authority checklist. In-scope reversible changes need a
  restorable backup, then you may proceed. Actions on the dangerous list are never reasonable; stop and ask
  the user before any tool call. A tool deny beats this packet.

### W1-B — libmpv embedding, low latency and Dahua specifics

- **Agent:** `research-budget`
- **Objective:** produce a concrete, version-correct recipe for libmpv 2.5.0 render API inside a GTK4
  `GtkGLArea` on Wayland/AMD, plus the lowest-latency RTSP option set and the Dahua endpoint/channel facts.
- **Scope:** mpv manual (render API, low-latency profile, RTSP options), libmpv headers on this machine,
  VA-API/EGL interop notes, Dahua RTSP/CGI/snapshot patterns, gtk4-layer-shell input-region API. Read-only.
- **Deliverable:** `.agent-work/reports/2026-11-08-w1b-video-research-budget.md` containing: a minimal
  call-order sketch (`mpv_create` → options → `loadfile` → render context → render/update callbacks →
  teardown), the exact option names that exist in 2.5.0, known pitfalls (GLES, FBO, flip, alpha, threading),
  a TCP-vs-UDP trade-off summary, and the Dahua URL/snapshot/channel facts with confidence.
- **Acceptance:** option names verified against the installed headers/manual, not memory; every claim has a
  URL or a local command; no invented API.
- **Stop:** if `gtk4paintablesink`/GStreamer becomes the better path, say so with evidence and cost.
- Change authority: follow the global change-authority checklist. In-scope reversible changes need a
  restorable backup, then you may proceed. Actions on the dangerous list are never reasonable; stop and ask
  the user before any tool call. A tool deny beats this packet.

### W1-C — Optional, only if W1-A/W1-B leave a gap

- **Agent:** `architect` (Kimi K3.1 Pro)
- **Objective:** challenge the proposed component boundaries and failure model with a compact evidence
  bundle (ARCHITECTURE.md §3–§8 + the two Wave 1 reports). One-hour scope, no code.
- **Deliverable:** `.agent-work/reports/2026-11-08-w1c-architecture-review-architect.md`, max 1 page:
  three risks the plan misses, three simplifications, and a clear keep/change recommendation.
- Change authority: follow the global change-authority checklist. In-scope reversible changes need a
  restorable backup, then you may proceed. Actions on the dangerous list are never reasonable; stop and ask
  the user before any tool call. A tool deny beats this packet.

## 4. Wave 2 spike packet (after Wave 1)

- **Agent:** `implementer-code`
- **Ownership:** `spike/` only (new directory). No edits outside `spike/`, no product code, no central files.
- **Deliverables:**
  1. `spike/layer_test.py` — Python GTK4 + Gtk4LayerShell window: 480×270, top-right, overlay layer,
     keyboard mode NONE, empty input region, distinctive test pattern and camera-name label; CLI flags for
     monitor, layer, input-region on/off.
  2. `spike/mpv_gl.c` + Makefile — minimal GTK4 `GtkGLArea` app using libmpv render API, `testsrc`/
     local file first, then an RTSP URL from a file descriptor or env (never argv), `hwdec=vaapi`, with
     `--benchmark` timing output.
  3. `spike/README.md` — how to run each.
- **Acceptance:** both build with the installed headers; layer test runs; mpv spike shows a stable image
  and reports first-frame timing; no credentials in source or argv.
- **Lead-run tests after delivery:** fullscreen matrix, `grim` pixel verification, focus/typing checks,
  click-through manual check, latency sampling, GPU/CPU sampling.
- **Stop:** if the GL path fails after two distinct fixes (context/API/flip), report the failure and the
  GStreamer fallback cost instead of thrashing.
- Change authority: follow the global change-authority checklist. In-scope reversible changes need a
  restorable backup, then you may proceed. Actions on the dangerous list are never reasonable; stop and ask
  the user before any tool call. A tool deny beats this packet.

## 5. Wave 3–5 implementation packets (shape)

Each packet names exact files, one owner, and a testable exit. Example split:

| Task | Owner | Files (exclusive) | Exit |
|---|---|---|---|
| M2 core | `implementer` | `src/main.c`, `src/app.*`, `src/config.*`, `src/camera.*`, `src/ipc.*`, `src/logging.*`, `src/util.*`, `Makefile`, `tests/test_config.c`, `tests/test_util.c` | `make check` green; CLI/status works headless |
| M3 video | `implementer-code` | `src/video.*`, `src/snapshot.*`, `tests/test_video.sh` | live camera in a normal window; first-frame timing |
| M4 overlay | `implementer-code` | `src/overlay.*`, `src/ui.*`, CSS | A3/A4/A5 evidence on Hyprland |
| M5 controls | `implementer` | `src/controls.*`, `src/app.c` (coordinated handoff), `docs/behaviour.md` | A6/A7/A8 timing report |
| M7 packaging | `implementer` | `packaging/*`, `examples/*`, `docs/*`, `integrations/*`, `README.md` | fresh-clone build/install test |

Rules: the lead hands `src/app.c` between owners explicitly; no concurrent edits; each agent runs
`make` and the relevant tests before reporting; the lead reruns the acceptance test independently.

## 6. Wave 6–7 verification packets

### QA (`tester`)
- Read-only on `src/`; may add files under `tests/` only with lead approval.
- Runs the acceptance matrix, 1-hour soak, 20-toggle leak test, reconnect test with the local RTSP harness,
  multi-monitor/fullscreen matrix, and reports exact commands/exit codes.
- Must not trust the implementation notes; verify from the outside (`ps`, `ss`, `status --json`, screenshots).

### Security (`security-reviewer`)
- Review credential flow end-to-end, log redaction, IPC permissions/peer check, config file modes, URL
  handling, error strings, core-dump exposure, and dependency surface.
- Deliver findings ranked by severity with a concrete reproduction or code reference; no code edits.

### Adversarial review (`reviewer-premium`)
- Review the overlay/video lifecycle for use-after-free, thread misuse, GL context lifetime, event races,
  reconnect storms and cleanup on every exit path.
- One pass, scoped to the critical files; no rewrite requests without a reproduction.

## 7. What the lead does personally

- Owns the desktop-interactive evidence: screenshots, focus checks, manual click-through, real fullscreen
  game tests, and the final judgement on whether the feel meets the brief.
- Integrates reports, updates `STATUS.md`/`TASKS.md`/`DECISIONS.md`/`RESEARCH.md`/`TESTS.md`/`HANDOFF.md`.
- Holds the credentials path and never lets an agent read or echo the secrets file.
- Decides GO/NO-GO after M1 and after each acceptance gate.
- Handles user communication and approvals; specialists do not ask the user directly.

## 8. Budget guardrails

- Default workhorse for implementation: DeepSeek V4.1 Flash.
- Two research calls only in Wave 1; no duplicate source gathering.
- One premium architecture/security review each, with a compact evidence bundle, not the whole repository.
- Keep 20–30% headroom for integration and test repair; if a model window is unknown, assume it is limited
  and keep premium calls short.
- Track actual usage only from visible `opencode.allowance` or user-supplied Console data; never invent it.

## 9. Current state

Wave 0 is complete. No specialist has been launched. The next action is the user's go-ahead for Wave 1,
which is document research only and changes no system state.

## 10. Execution outcome (2026-11-08)

All waves completed. Research and spike products are committed under `spike/` or reported in
`.agent-work/reports/`; M2-M4 and docs/packaging were implemented by Sonnet/Haiku subagents and independently
re-verified by the lead. Both reviews (security and lifecycle) were completed and their reports now include a
**Resolution** table. High-value findings were fixed with regression tests; the remaining items (F3, F7, F10-F12,
S5, S11, S13) are documented in `ARCHITECTURE.md` §13 and the handoff rather than rushed. No subagent is running.
