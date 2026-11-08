#!/usr/bin/env python3
"""Disposable GTK4 + Gtk4LayerShell probe for the CCTV overlay feasibility spike.

This is not product code. It creates a small, deterministic layer-shell surface so
a human can verify visibility above fullscreen windows, click-through, focus
behaviour, and per-monitor placement.
"""

import argparse
import signal
import sys
import time

import cairo
import gi

gi.require_version("Gdk", "4.0")
gi.require_version("Gtk", "4.0")
gi.require_version("Gtk4LayerShell", "1.0")

from gi.repository import Gdk, Gtk, Gtk4LayerShell, GLib


NAMESPACE = "cctv-overlay-spike"


def parse_size(s: str):
    w, h = s.split("x", 1)
    return max(1, int(w)), max(1, int(h))


def parse_args(argv=None):
    p = argparse.ArgumentParser(
        description="GTK4+Gtk4LayerShell overlay probe with deterministic pixels."
    )
    p.add_argument(
        "--size",
        type=parse_size,
        default="480x270",
        help="Window size as WxH (default 480x270).",
    )
    p.add_argument(
        "--corner",
        choices=("top-right", "top-left", "bottom-right", "bottom-left"),
        default="top-right",
        help="Corner to anchor the overlay (default top-right).",
    )
    p.add_argument(
        "--margin",
        type=int,
        default=24,
        help="Margin from the anchored edges in pixels (default 24).",
    )
    p.add_argument(
        "--monitor",
        default="focused",
        help="Connector name (e.g. DP-1) or 'focused' (default focused).",
    )
    p.add_argument(
        "--layer",
        choices=("overlay", "top"),
        default="overlay",
        help="Layer-shell layer (default overlay).",
    )
    p.add_argument(
        "--input",
        choices=("pass", "none"),
        default="none",
        help="'none' sets an empty input region; 'pass' leaves it alone (default none).",
    )
    p.add_argument(
        "--duration",
        type=float,
        default=0,
        help="Seconds to keep the window open (0 = until Ctrl+C, default 0).",
    )
    p.add_argument(
        "--smoke",
        action="store_true",
        help="Show for ~1 second and exit 0.",
    )
    return p.parse_args(argv)


def list_monitors(display: Gdk.Display):
    model = display.get_monitors()
    return [model.get_item(i) for i in range(model.get_n_items())]


def pick_monitor(display: Gdk.Display, request: str):
    """Return a Gdk.Monitor matching request, or the focused/primary one."""
    monitors = list_monitors(display)
    if not monitors:
        raise RuntimeError("no monitors reported by GDK")

    if request.lower() == "focused":
        # GTK has no portable "focused monitor" API; try to find the monitor under
        # the pointer, otherwise fall back to the first monitor.
        seat = display.get_default_seat()
        if seat is not None:
            pointer = seat.get_pointer()
            if pointer is not None:
                try:
                    surf, px, py = pointer.get_surface_at_position()
                    del surf  # not needed; we only care about the coordinates
                    for i, mon in enumerate(monitors):
                        geom = mon.get_geometry()
                        if geom.contains_point(int(px), int(py)):
                            return mon, i
                except Exception as exc:  # noqa: BLE001
                    print(f"pointer position lookup failed: {exc}")
        return monitors[0], 0

    request_up = request.upper()
    for i, mon in enumerate(monitors):
        if (mon.get_connector() or "").upper() == request_up:
            return mon, i
    return monitors[0], 0


def describe_monitor(mon: Gdk.Monitor, index: int):
    geom = mon.get_geometry()
    return (
        f"index={index} connector={mon.get_connector()!r} "
        f"geometry=({geom.x},{geom.y} {geom.width}x{geom.height}) "
        f"scale={mon.get_scale_factor()}"
    )


def draw_pattern(da: Gtk.DrawingArea, cr: cairo.Context, width: int, height: int, _=None):
    """Draw a deterministic magenta/green/white test pattern."""
    # Magenta background.
    cr.set_source_rgb(1.0, 0.0, 1.0)
    cr.paint()

    # Solid green square at (20,20) 80x80.
    cr.set_source_rgb(0.0, 1.0, 0.0)
    cr.rectangle(20, 20, 80, 80)
    cr.fill()

    # 1px white border.
    cr.set_source_rgb(1.0, 1.0, 1.0)
    cr.set_line_width(1.0)
    cr.rectangle(0.5, 0.5, width - 1, height - 1)
    cr.stroke()

    # Label.
    cr.set_source_rgb(1.0, 1.0, 1.0)
    cr.select_font_face("sans-serif", cairo.FONT_SLANT_NORMAL, cairo.FONT_WEIGHT_BOLD)
    cr.set_font_size(24)
    text = "LAYER TEST"
    extents = cr.text_extents(text)
    cr.move_to((width - extents.width) / 2, (height + extents.height) / 2)
    cr.show_text(text)


def try_set_empty_input_region(window: Gtk.Window) -> tuple[bool, str]:
    """Best-effort empty input region. Returns (ok, message)."""
    native = window.get_native()
    if native is None:
        return False, "no native surface yet (window not realized)"
    surface = native.get_surface()
    if surface is None:
        return False, "no surface yet"
    try:
        empty = cairo.Region()
        surface.set_input_region(empty)
        return True, f"surface={surface}"
    except Exception as exc:  # noqa: BLE001
        return False, f"set_input_region failed: {exc}"


def anchor_for_corner(corner: str):
    edge_map = {
        "top": Gtk4LayerShell.Edge.TOP,
        "bottom": Gtk4LayerShell.Edge.BOTTOM,
        "left": Gtk4LayerShell.Edge.LEFT,
        "right": Gtk4LayerShell.Edge.RIGHT,
    }
    anchors = {"top": False, "bottom": False, "left": False, "right": False}
    for part in corner.split("-"):
        anchors[part] = True
    return {edge_map[k]: v for k, v in anchors.items()}


def main(argv=None):
    args = parse_args(argv)
    duration = 1.0 if args.smoke else args.duration

    app = Gtk.Application(application_id=f"com.example.{NAMESPACE}")

    def on_activate(_app):
        layer_supported = Gtk4LayerShell.is_supported()
        print(f"layer-shell supported: {layer_supported}")

        win = Gtk.Window(application=_app)
        win.set_title("Layer Test")
        win.set_default_size(args.size[0], args.size[1])
        win.set_resizable(False)
        win.set_can_focus(False)
        win.set_focusable(False)
        # Do not call present()/grab_focus(); map the window indirectly.

        display = Gdk.Display.get_default()
        monitor, mon_index = pick_monitor(display, args.monitor)
        print(f"chosen monitor: {describe_monitor(monitor, mon_index)}")

        da = Gtk.DrawingArea()
        da.set_content_width(args.size[0])
        da.set_content_height(args.size[1])
        da.set_draw_func(draw_pattern)
        win.set_child(da)

        # Pointer probe: logs any pointer event this surface actually receives.
        # With an empty input region the compositor must deliver none of these.
        motion = Gtk.EventControllerMotion()
        motion.connect("enter", lambda _c, x, y: print(f"POINTER ENTER at {x:.0f},{y:.0f}", flush=True))
        motion.connect("motion", lambda _c, x, y: print(f"POINTER MOTION {x:.0f},{y:.0f}", flush=True))
        motion.connect("leave", lambda _c: print("POINTER LEAVE", flush=True))
        da.add_controller(motion)
        click = Gtk.GestureClick()
        click.set_button(0)
        click.connect("pressed", lambda _g, n, x, y: print(f"POINTER CLICK at {x:.0f},{y:.0f}", flush=True))
        da.add_controller(click)

        if layer_supported:
            Gtk4LayerShell.init_for_window(win)
            Gtk4LayerShell.set_namespace(win, NAMESPACE)
            Gtk4LayerShell.set_monitor(win, monitor)
            Gtk4LayerShell.set_exclusive_zone(win, -1)
            Gtk4LayerShell.set_keyboard_mode(win, Gtk4LayerShell.KeyboardMode.NONE)
            layer = (
                Gtk4LayerShell.Layer.OVERLAY
                if args.layer == "overlay"
                else Gtk4LayerShell.Layer.TOP
            )
            Gtk4LayerShell.set_layer(win, layer)
            for edge, anchor in anchor_for_corner(args.corner).items():
                Gtk4LayerShell.set_anchor(win, edge, anchor)
            Gtk4LayerShell.set_margin(win, Gtk4LayerShell.Edge.TOP, args.margin)
            Gtk4LayerShell.set_margin(win, Gtk4LayerShell.Edge.BOTTOM, args.margin)
            Gtk4LayerShell.set_margin(win, Gtk4LayerShell.Edge.LEFT, args.margin)
            Gtk4LayerShell.set_margin(win, Gtk4LayerShell.Edge.RIGHT, args.margin)
            print(
                f"layer-shell config: namespace={NAMESPACE} layer={args.layer} "
                f"corner={args.corner} margin={args.margin}"
            )
        else:
            print("warning: layer-shell not supported; falling back to normal window")
            win.set_decorated(False)

        input_mode = args.input
        print(f"input mode: {input_mode}")

        def reapply_input_region():
            ok, msg = try_set_empty_input_region(win)
            status = "OK" if ok else "FAILED"
            return status, msg

        def on_map(_w):
            if input_mode == "none":
                status, msg = reapply_input_region()
                print(f"empty input region: {status} ({msg})")
            else:
                print("input region: pass-through (unchanged)")

        def on_size_changed(_obj, _pspec):
            if input_mode == "none":
                status, msg = reapply_input_region()
                print(f"empty input region reapply on resize: {status} ({msg})")

        win.connect("map", on_map)
        win.connect("notify::default-width", on_size_changed)
        win.connect("notify::default-height", on_size_changed)

        # set_visible(True) maps the window without an explicit present()/focus grab.
        win.set_visible(True)
        print(
            f"window shown: size={args.size[0]}x{args.size[1]} "
            f"decorated={win.get_decorated()} focusable={win.get_focusable()}"
        )

        if duration > 0:
            # Add one extra second for durations <= 1s so short smoke tests reliably paint.
            delay_ms = int(max(duration, 0.1) * 1000) + (500 if duration <= 1 else 0)
            GLib.timeout_add(delay_ms, app.quit)

    app.connect("activate", on_activate)

    # Allow clean Ctrl+C under a running GTK main loop.
    signal.signal(signal.SIGINT, lambda _s, _f: app.quit())

    print("starting main loop (Ctrl+C to stop)...")
    t0 = time.monotonic()
    status = app.run([])
    elapsed = time.monotonic() - t0
    print(f"main loop exited after {elapsed:.3f}s with status {status}")
    return 0 if status == 0 else status


if __name__ == "__main__":
    sys.exit(main())
