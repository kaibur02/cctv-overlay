# ADR-0002: Use libmpv's render API inside GTK4

- **Status:** Proposed — pending Milestone 1 evidence
- **Date:** 2026-11-08
- **Deciders:** project lead
- **Related:** ARCHITECTURE.md §2.2, §3.3

## Context

The app needs low-latency RTSP playback of Dahua H.264/H.265 streams on AMD (VA-API), embedded in an
overlay window, with the player fully released when hidden. The existing project uses Android Media3;
the Linux equivalent should use an established player rather than a custom RTSP/decoder stack.

Installed on the target: `mpv` 0.41.0 / libmpv 2.5.0 with `client.h`, `render.h`, `render_gl.h`;
GStreamer 1.28.8 **without** `gtk4paintablesink` or VA decoders; FFmpeg 9.0.2; GTK4 4.24.1.

## Options considered

1. **libmpv render API into `GtkGLArea`** — mature RTSP, hardware decode, low-latency profile, full control
   of the texture path, no extra packages.
2. **GStreamer + `gtk4paintablesink`** — idiomatic GTK rendering, but missing plugins on this system;
   requires installs and more reconnect/state code.
3. **mpv `--wid` embedding** — X11/Windows only; not available on Wayland.
4. **FFmpeg decode + custom GL upload** — reimplements sync, buffering, reconnect; too much for v0.1.
5. **Separate mpv window** — cannot be made click-through/focusless as reliably; breaks overlay atomicity.

## Decision

Adopt option 1: one `mpv_handle` + `mpv_render_context` per active stream, `vo=libmpv`, `hwdec=vaapi`,
`profile=low-latency`, no audio, RTSP over TCP by default (configurable). Render into the `GtkGLArea` FBO;
schedule redraws from the render update callback via the GTK main loop; free the render context and
terminate the player on hide.

Keep the video engine behind a small `video_engine` interface so GStreamer can replace it if the GL path
proves unstable.

## Consequences

- No new runtime packages; VA-API is already confirmed.
- We own the GL context/FBO/flip/alpha details and must test them (S4).
- Credentials appear in the in-memory RTSP URL (unavoidable with libavformat); logging and argv rules must
  compensate.
- A player instance is created on show and destroyed on hide; startup cost is part of the latency budget.

## Revisit when

- M1 shows unstable rendering, tearing, colour/flip problems or excessive CPU.
- A maintained GTK4 GStreamer sink becomes available and reconnect handling is simpler.
- Multi-camera grid (future) needs more than one decoder and a different resource model.
