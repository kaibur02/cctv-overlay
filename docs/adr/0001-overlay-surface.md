# ADR-0001: Use wlr-layer-shell for the overlay surface

- **Status:** Accepted (M1 evidence 2026-11-08: overlay layer visible over true fullscreen/maximized/tiled on Hyprland 0.56.2; no focus change; typing reaches the fullscreen app; empty input region delivers no pointer events; layer must be OVERLAY not TOP; gtk4-layer-shell must load before libwayland)
- **Date:** 2026-11-08
- **Deciders:** project lead, owner
- **Related:** ARCHITECTURE.md §2.1, `.agent-work/TESTS.md`

## Context

The overlay must stay above ordinary and fullscreen windows, take no keyboard focus, pass mouse clicks
through, have no decorations, and not change workspaces. The target is Hyprland 0.56.2 on Wayland, with
portability to other compositors where practical.

A normal Wayland toplevel cannot portably satisfy these: the compositor controls stacking, fullscreen
windows are above normal windows, focus is normally taken on map, and a toplevel cannot make its input
region empty without platform-specific work.

## Options considered

1. **wlr-layer-shell `overlay` layer** — compositor-managed surface above windows; explicit keyboard
   interactivity; separate input region; no decorations.
2. **Floating toplevel + Hyprland window rules** — portable API but fullscreen still covers it, focus and
   click-through are unreliable.
3. **Hyprland plugin** — perfect integration, private ABI, Hyprland-only, high maintenance.
4. **Special workspace / pinned window** — does not stay above fullscreen, changes workspace semantics.

## Decision

Adopt option 1 as the primary overlay backend: GTK4 + `gtk4-layer-shell`, layer `overlay`, namespace
`cctv-overlay`, keyboard interactivity `NONE`, empty input region, anchors/margins from config. Provide a
documented normal-window fallback (`--no-layer-shell`) for compositors without layer-shell, with reduced
guarantees (no reliable fullscreen stacking, click-through best-effort).

## Consequences

- Works on Hyprland, Sway/wlroots, KDE Plasma Wayland; not GNOME or X11 (gtk4-layer-shell's documented
  support boundary).
- Fullscreen visibility and direct-scanout behaviour must be proven empirically on Hyprland (S1/S2).
- The overlay never participates in tiling, focus or workspace state, which is the desired UX.
- The overlay backend is isolated behind `overlay.c`; the rest of the app is compositor-agnostic.

## Revisit when

- M1 shows layer surfaces are hidden by a fullscreen window on the target compositor.
- A compositor offers a better supported always-on-top/click-through primitive.
- The project needs X11 or GNOME support as a first-class target.
