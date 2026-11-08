# Feasibility spike (disposable)

Two throwaway probes for the CCTV-overlay feasibility question. They are **not**
product code and are expected to be deleted once the spike is decided.

| File | Purpose |
| --- | --- |
| `layer_test.py` | GTK4 + gtk4-layer-shell: can we put a small, non-interactive surface above fullscreen apps? |
| `mpv_gl.c` | GTK4 `GtkGLArea` + libmpv render API: can we decode (VA-API) and draw live video into our own widget? |
| `Makefile` | Builds `mpv_gl` only. |

Test media lives outside the repo, in `/tmp/opencode/cctv-spike/` (ephemeral).

## Host baseline used for these runs

Wayland session (`wayland-1`), GTK 4.24.1, libmpv/mpv 0.41.0, libepoxy 1.5.10,
ffmpeg n9.0.2, Mesa 26.2.5 on an AMD RX 9070 XT (gfx1201). `vainfo` reports
H.264 and HEVC Main/Main10 VLD entrypoints on this machine.

Nothing here needs installation on this host; do not run package commands as
part of the spike.

## Build

```bash
make -C spike          # produces spike/mpv_gl
make -C spike clean
```

## Test clips

```bash
mkdir -p /tmp/opencode/cctv-spike && cd /tmp/opencode/cctv-spike

# 4 s 704x576 25 fps H.264 (Constrained Baseline, yuv420p)
ffmpeg -hide_banner -loglevel error -y -f lavfi \
  -i testsrc=size=704x576:rate=25 -t 4 \
  -c:v libx264 -preset ultrafast -pix_fmt yuv420p -g 25 \
  -movflags +faststart test.mp4

# 4 s 704x576 25 fps HEVC (Main, yuv420p)
ffmpeg -hide_banner -loglevel error -y -f lavfi \
  -i testsrc=size=704x576:rate=25 -t 4 \
  -c:v libx265 -preset ultrafast -pix_fmt yuv420p -tag:v hvc1 \
  -x265-params log-level=error:keyint=25 \
  -movflags +faststart test_hevc.mp4

# The probe takes no URL on argv; it reads exactly one line from a file.
printf '%s\n' /tmp/opencode/cctv-spike/test.mp4      > url.txt
printf '%s\n' /tmp/opencode/cctv-spike/test_hevc.mp4 > url_hevc.txt
```

`url.txt` may hold a plain path or a URL (`rtsp://…`). Keep real camera
credentials out of this repo and out of shell history; the probe only reads the
file, and this spike was tested with local files only.

## `mpv_gl` — GL + libmpv probe

```bash
# Benchmark (untimed) H.264, VA-API decode
timeout 15 ./spike/mpv_gl --url-file /tmp/opencode/cctv-spike/url.txt \
  --duration 3 --benchmark --hwdec vaapi

# Real-time playback (representative latency)
timeout 15 ./spike/mpv_gl --url-file /tmp/opencode/cctv-spike/url.txt \
  --duration 3 --hwdec vaapi

# HEVC
timeout 15 ./spike/mpv_gl --url-file /tmp/opencode/cctv-spike/url_hevc.txt \
  --duration 3 --benchmark --hwdec vaapi
```

Flags:

| Flag | Meaning |
| --- | --- |
| `--url-file PATH` | **Required.** File containing one playback source (plain path or URL). |
| `--duration SEC` | Wall-clock seconds from main-loop start before the probe quits. `0` (default) = until EOF / Ctrl+C. EOF on a finite clip also ends the probe early. |
| `--benchmark` | Sets mpv `untimed=yes`: ignores frame display timing, so the clip is decoded/rendered as fast as the GTK frame clock allows. Do not read its wall-clock runtime as playback duration. |
| `--hwdec MODE` | mpv `hwdec` value: `vaapi` (default), `vaapi-copy`, `auto-safe`, `no`, … |
| `--rtsp-transport tcp\|udp` | mpv `rtsp-transport` (default `tcp`). |
| `--size WxH` | Window size (default `960x540`). The video is scaled to the widget. |

What it prints (stdout, unbuffered):

- `render context: created` / `including WL_DISPLAY` — GL renderer init, and
  whether the Wayland display was handed to mpv (needed for VA-API/GL interop).
- `file-loaded latency` — `loadfile` command → `MPV_EVENT_FILE_LOADED`.
- `first-frame latency (loadfile -> first rendered frame)` — `loadfile` command →
  first `mpv_render_context_render()` that consumed a signalled video frame.
- `video-params` / `container-fps` / `video-codec` — geometry, container fps and
  codec string (mpv 0.41 has no `fps` key inside `video-params`).
- `hwdec-current: vaapi` — proof that hardware decoding is actually active
  (`vaapi-loop`, `vaapi-copy`, `(none)` mean something else).
- `pixfmt=vaapi` — the decoder output is a VA-API surface.
- `--- summary ---` — elapsed main-loop time, frames rendered, render rate,
  video params and final `hwdec-current`.
- mpv warnings/errors go through `mpv_request_log_messages("warn")` and are
  prefixed `mpv [...]`.

Exit status: `0` normal, `2` GL/render-context setup failed, `3` playback failed
before any frame was rendered, `124`/`137` if `timeout` had to kill it (a hang).
`Cannot load libcuda.so.1`, `WARNING: radv is not a conformant Vulkan
implementation`, and mpv `vo/gpu`/hwdec probing messages are benign noise on
this host.

## `layer_test.py` — layer-shell probe

```bash
python3 spike/layer_test.py --smoke                      # ~1 s, sanity check
python3 spike/layer_test.py --duration 10 --corner top-right --margin 24 \
  --monitor focused --layer overlay --input none
```

| Flag | Meaning |
| --- | --- |
| `--size WxH` | Window size (default `480x270`). |
| `--corner top-right\|top-left\|bottom-right\|bottom-left` | Anchored corner (default `top-right`). |
| `--margin N` | Margin from anchored edges in px (default `24`). |
| `--monitor NAME\|focused` | Connector (e.g. `DP-1`) or the monitor under the pointer. |
| `--layer overlay\|top` | Layer-shell layer (default `overlay`). |
| `--input none\|pass` | `none` sets an empty input region (click-through); `pass` leaves it alone. |
| `--duration SEC` | Keep the window for N seconds (`0` = until Ctrl+C). |
| `--smoke` | Show ~1 s and exit 0. |

The window is deliberately never `present()`ed or focused, and
`can-focus`/`focusable` are `false`. It prints the chosen monitor, layer-shell
support, input-region status (~`OK (surface=…)`) and whether the empty input
region was reapplied after a resize.

## Manual checks for the lead

`layer_test.py` (this is the part a human has to judge):

1. Surface is visible **above a fullscreen app/video** on the chosen monitor,
   with the layer set to `overlay` (and compare with `top`).
2. It does **not** take keyboard focus and does **not** steal focus on map.
3. Clicks/scrolls go **through** it with `--input none`.
4. Anchor/margin/connector placement is correct on a multi-monitor setup, and
   the surface stays put when the window is resized (`notify::default-size`).
5. `hyprctl layers` shows namespace `cctv-overlay-spike` on the expected
   monitor/layer.

`mpv_gl`:

1. The clip is actually visible, upright (not vertically flipped), not
   letterboxed incorrectly, and colours look right for BT.601 SD content.
2. `hwdec-current: vaapi` and `pixfmt=vaapi` for both H.264 and HEVC.
3. Closing the window (WM close) and the `--duration` timeout both tear down
   cleanly: `render context: freed`, exit 0, no crash/hang, no leaked window.
4. Whether this plain toplevel could later become a layer-shell surface
   (`gtk4-layer-shell` init on the same window) — not implemented here.

## Known limitations

- `mpv_gl` is a normal toplevel, **not** a layer-shell surface; combining
  layer-shell with a GL video widget is the open question.
- The render path is OpenGL/EGL via libepoxy, aimed at Wayland. The X11/GLX
  path is untested (it would need `gdk_gl_context_get_proc_address`).
- Benchmark mode is bounded by GTK's frame clock and window compositing, so
  "render rate" is end-to-end, not decoder-only throughput.
- First-frame latency is measured from the `loadfile` command, includes window
  realise/demuxer/decoder startup, and is a single sample, not a distribution.
- No transparency/alpha blending, no OSD, no audio, no input handling, no
  click-through: those are later product questions.
- EOF always ends the probe, even when `--duration` is larger than the clip.
- `layer_test.py` re-applies the empty input region on `notify::default-*`;
  live compositor resizes may still need a re-check.
- No real camera/RTSP endpoint was contacted; the RTSP options are wired but
  only local files were exercised.
