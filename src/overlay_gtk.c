#define _GNU_SOURCE
#include "overlay.h"
#include "overlay_gtk.h"

#include "logging.h"
#include "video.h"

#include <epoxy/gl.h>
#include <gdk/gdk.h>
#include <gtk/gtk.h>
#include <gtk4-layer-shell/gtk4-layer-shell.h>

#include <malloc.h>
#include <string.h>

#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/wayland/gdkwayland.h>
#endif

#define OVERLAY_ERROR (g_quark_from_static_string("cctv-overlay-gtk-error"))

static struct {
	gboolean inited;
	GtkWidget *win, *gl, *chip, *dot, *name, *status;
	gboolean show_label, show_status;
	AppState state;
	OverlayRenderFn render_fn;
	void *render_user;
} O;

static const char *const STATE_CLASS[] = { "hidden", "connecting", "live", "reconnecting", "error" };

static const char CSS[] =
	"window.cctv-overlay { background: transparent; }\n"
	".cctv-frame { background: #000; border-radius: 8px; }\n"
	".cctv-chip { background: rgba(0,0,0,0.55); border-radius: 6px; padding: 2px 8px; margin: 6px; }\n"
	".cctv-name { color: #fff; font-size: 12px; font-weight: 600; }\n"
	".cctv-dot { min-width: 9px; min-height: 9px; border-radius: 5px; background: #888; }\n"
	".cctv-dot.connecting, .cctv-dot.reconnecting { background: #f0a500; }\n"
	".cctv-dot.live { background: #2ecc71; }\n"
	".cctv-dot.error { background: #e74c3c; }\n"
	".cctv-status { color: #fff; background: rgba(0,0,0,0.6); border-radius: 6px; padding: 2px 8px;"
	" margin: 8px; font-size: 11px; }\n";

/* ---------------- init ---------------- */

gboolean overlay_init(GError **err)
{
	if (O.inited)
		return TRUE;

	gdk_set_allowed_backends("wayland"); /* layer-shell is Wayland-only; never fall back to X11 */
	if (!gtk_init_check()) {
		g_set_error_literal(err, OVERLAY_ERROR, 1,
				    "cannot open a Wayland display (is WAYLAND_DISPLAY set and the compositor running?)");
		return FALSE;
	}
	GdkDisplay *d = gdk_display_get_default();
	if (!d || !GDK_IS_WAYLAND_DISPLAY(d)) {
		g_set_error_literal(err, OVERLAY_ERROR, 1, "the display is not Wayland; the overlay needs a Wayland compositor");
		return FALSE;
	}
	if (!gtk_layer_is_supported()) {
		g_set_error_literal(err, OVERLAY_ERROR, 1,
				    "layer-shell is not available: the compositor lacks zwlr_layer_shell_v1, or "
				    "libgtk4-layer-shell was linked after libwayland (link order; see docs/overlay.md)");
		return FALSE;
	}
	/* Self-check: a window we turn into a layer surface must really become one. */
	GtkWidget *probe = gtk_window_new();
	gtk_layer_init_for_window(GTK_WINDOW(probe));
	gboolean is_layer = gtk_layer_is_layer_window(GTK_WINDOW(probe));
	gtk_window_destroy(GTK_WINDOW(probe));
	if (!is_layer) {
		g_set_error_literal(err, OVERLAY_ERROR, 1,
				    "gtk4-layer-shell did not take over the window (library load order problem; "
				    "link gtk4-layer-shell before gtk4/wayland, see docs/overlay.md)");
		return FALSE;
	}

	GtkCssProvider *p = gtk_css_provider_new();
	gtk_css_provider_load_from_string(p, CSS);
	gtk_style_context_add_provider_for_display(d, GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(p);

	O.inited = TRUE;
	log_debug("overlay: GTK %u.%u.%u, layer-shell protocol v%u ready", gtk_get_major_version(),
		  gtk_get_minor_version(), gtk_get_micro_version(), gtk_layer_get_protocol_version());
	return TRUE;
}

/* ---------------- helpers ---------------- */

GtkGLArea *overlay_gtk_gl_area(void) { return O.gl ? GTK_GL_AREA(O.gl) : NULL; }

void overlay_gtk_set_render_fn(OverlayRenderFn fn, void *user)
{
	O.render_fn = fn;
	O.render_user = user;
}

static void apply_empty_input_region(GtkWidget *w)
{
	GdkSurface *s = gtk_native_get_surface(GTK_NATIVE(w));
	if (!s)
		return;
	cairo_region_t *empty = cairo_region_create();
	gdk_surface_set_input_region(s, empty);
	cairo_region_destroy(empty);
}

static void on_surface_layout(GdkSurface *s, int w, int h, gpointer data)
{
	(void)s; (void)w; (void)h;
	apply_empty_input_region(GTK_WIDGET(data)); /* re-apply on every resize */
}

static void on_win_realize(GtkWidget *w, gpointer data)
{
	(void)data;
	GdkSurface *s = gtk_native_get_surface(GTK_NATIVE(w));
	if (s)
		g_signal_connect(s, "layout", G_CALLBACK(on_surface_layout), w);
	apply_empty_input_region(w);
}

static void on_win_map(GtkWidget *w, gpointer data)
{
	(void)data;
	apply_empty_input_region(w);
}

static void on_win_destroy(GtkWidget *w, gpointer data)
{
	(void)data;
	if (O.win == w) { /* destroyed behind our back (compositor closed the layer surface) */
		log_warn("overlay: window destroyed by the compositor or toolkit");
		O.win = O.gl = O.chip = O.dot = O.name = O.status = NULL;
	}
}

static gboolean on_gl_render(GtkGLArea *area, GdkGLContext *ctx, gpointer data)
{
	(void)ctx; (void)data;
	if (O.render_fn)
		return O.render_fn(area, O.render_user);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	return TRUE;
}

static GdkMonitor *find_monitor(const char *spec)
{
	/* "focused": GTK has no portable API for the focused output, so leave the output unset and
	 * let the compositor choose (Hyprland picks the focused monitor). NULL == that choice. */
	if (!spec || strcmp(spec, "focused") == 0)
		return NULL;

	GListModel *mons = gdk_display_get_monitors(gdk_display_get_default());
	GdkMonitor *found = NULL;
	for (guint i = 0; i < g_list_model_get_n_items(mons) && !found; i++) {
		GdkMonitor *m = g_list_model_get_item(mons, i);
		const char *conn = gdk_monitor_get_connector(m);
		if (strcmp(spec, "primary") == 0 || (conn && strcmp(conn, spec) == 0))
			found = m; /* "primary": the first output in the list */
		else
			g_object_unref(m);
	}
	if (!found)
		log_warn("overlay: monitor '%s' not found; letting the compositor choose", spec);
	else
		g_object_unref(found); /* the model keeps it alive; we only need the pointer briefly */
	return found;
}

static void set_status_text(const char *text)
{
	if (!O.status)
		return;
	gboolean show = text && *text && O.show_status;
	gtk_label_set_text(GTK_LABEL(O.status), show ? text : "");
	gtk_widget_set_visible(O.status, show);
}

static void refresh_chip(void)
{
	if (!O.chip)
		return;
	gtk_widget_set_visible(O.dot, O.show_status);
	gtk_widget_set_visible(O.name, O.show_label);
	gtk_widget_set_visible(O.chip, O.show_label || O.show_status);
}

/* ---------------- interface ---------------- */

void overlay_show(const Config *cfg, const char *label)
{
	if (O.win) {
		overlay_set_label(label);
		return;
	}
	if (!O.inited) {
		log_error("overlay: overlay_show() before overlay_init()");
		return;
	}

	O.show_label = cfg->show_label;
	O.show_status = cfg->show_status;

	GtkWindow *win = GTK_WINDOW(gtk_window_new());
	O.win = GTK_WIDGET(win);
	gtk_window_set_title(win, "cctv-overlay");
	gtk_window_set_decorated(win, FALSE);
	gtk_window_set_resizable(win, FALSE);
	gtk_window_set_default_size(win, cfg->width, cfg->height);
	gtk_widget_add_css_class(O.win, "cctv-overlay");
	gtk_widget_set_focusable(O.win, FALSE);
	gtk_widget_set_can_target(O.win, FALSE);

	gtk_layer_init_for_window(win);
	if (!gtk_layer_is_layer_window(win)) {
		log_error("overlay: window did not become a layer surface; not showing it");
		gtk_window_destroy(win);
		O.win = NULL;
		return;
	}
	gtk_layer_set_namespace(win, "cctv-overlay");
	gtk_layer_set_layer(win, strcmp(cfg->layer, "top") == 0 ? GTK_LAYER_SHELL_LAYER_TOP
								: GTK_LAYER_SHELL_LAYER_OVERLAY);
	gtk_layer_set_keyboard_mode(win, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
	gtk_layer_set_exclusive_zone(win, -1);
	gboolean top = g_str_has_prefix(cfg->anchor, "top"), left = g_str_has_suffix(cfg->anchor, "left");
	gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_TOP, top);
	gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_BOTTOM, !top);
	gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_LEFT, left);
	gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_RIGHT, !left);
	gtk_layer_set_margin(win, top ? GTK_LAYER_SHELL_EDGE_TOP : GTK_LAYER_SHELL_EDGE_BOTTOM, cfg->margin_y);
	gtk_layer_set_margin(win, left ? GTK_LAYER_SHELL_EDGE_LEFT : GTK_LAYER_SHELL_EDGE_RIGHT, cfg->margin_x);
	gtk_layer_set_monitor(win, find_monitor(cfg->monitor));

	/* Content: GL area (video) with a small label + status dot and a status line on top. */
	GtkWidget *frame = gtk_overlay_new();
	gtk_widget_add_css_class(frame, "cctv-frame");
	gtk_widget_set_overflow(frame, GTK_OVERFLOW_HIDDEN); /* clips the video to the rounded corners */

	O.gl = gtk_gl_area_new();
	gtk_gl_area_set_has_depth_buffer(GTK_GL_AREA(O.gl), FALSE);
	gtk_gl_area_set_has_stencil_buffer(GTK_GL_AREA(O.gl), FALSE);
	gtk_gl_area_set_allowed_apis(GTK_GL_AREA(O.gl), GDK_GL_API_GL | GDK_GL_API_GLES);
	gtk_widget_set_hexpand(O.gl, TRUE);
	gtk_widget_set_vexpand(O.gl, TRUE);
	g_signal_connect(O.gl, "render", G_CALLBACK(on_gl_render), NULL);
	gtk_overlay_set_child(GTK_OVERLAY(frame), O.gl);

	O.chip = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_add_css_class(O.chip, "cctv-chip");
	gtk_widget_set_halign(O.chip, GTK_ALIGN_START);
	gtk_widget_set_valign(O.chip, GTK_ALIGN_START);
	O.dot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_add_css_class(O.dot, "cctv-dot");
	gtk_widget_set_valign(O.dot, GTK_ALIGN_CENTER);
	O.name = gtk_label_new(label);
	gtk_widget_add_css_class(O.name, "cctv-name");
	gtk_label_set_ellipsize(GTK_LABEL(O.name), PANGO_ELLIPSIZE_END);
	gtk_label_set_max_width_chars(GTK_LABEL(O.name), 32);
	gtk_box_append(GTK_BOX(O.chip), O.dot);
	gtk_box_append(GTK_BOX(O.chip), O.name);
	gtk_overlay_add_overlay(GTK_OVERLAY(frame), O.chip);

	O.status = gtk_label_new("");
	gtk_widget_add_css_class(O.status, "cctv-status");
	gtk_widget_set_halign(O.status, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(O.status, GTK_ALIGN_END);
	gtk_label_set_ellipsize(GTK_LABEL(O.status), PANGO_ELLIPSIZE_END);
	gtk_label_set_max_width_chars(GTK_LABEL(O.status), 48);
	gtk_overlay_add_overlay(GTK_OVERLAY(frame), O.status);

	/* Nothing in the overlay takes pointer input, in addition to the empty input region. */
	GtkWidget *w[] = { frame, O.gl, O.chip, O.dot, O.name, O.status };
	for (guint i = 0; i < G_N_ELEMENTS(w); i++) {
		gtk_widget_set_can_target(w[i], FALSE);
		gtk_widget_set_focusable(w[i], FALSE);
	}
	gtk_window_set_child(win, frame);

	g_signal_connect(win, "destroy", G_CALLBACK(on_win_destroy), NULL);
	g_signal_connect_after(win, "realize", G_CALLBACK(on_win_realize), NULL);
	g_signal_connect_after(win, "map", G_CALLBACK(on_win_map), NULL);

	O.state = APP_CONNECTING;
	gtk_widget_remove_css_class(O.dot, "connecting");
	gtk_widget_add_css_class(O.dot, "connecting");
	set_status_text("Connecting…");
	refresh_chip();

	/* gtk_widget_set_visible() maps without the activation/focus request of gtk_window_present(). */
	gtk_widget_realize(GTK_WIDGET(win));
	gtk_widget_realize(O.gl);
	gtk_widget_set_visible(GTK_WIDGET(win), TRUE);
	log_debug("overlay: shown %dx%d anchor=%s monitor=%s layer=%s", cfg->width, cfg->height, cfg->anchor,
		  cfg->monitor, cfg->layer);
}

static void trim_idle(gpointer data)
{
	(void)data;
	malloc_trim(0); /* hand freed pages (mpv, GL driver, GTK) back while the overlay is hidden */
}

void overlay_hide(void)
{
	if (!O.win)
		return;
	video_stop(); /* render context must die while the GL context is alive */
	GtkWidget *w = O.win;
	O.win = O.gl = O.chip = O.dot = O.name = O.status = NULL;
	gtk_window_destroy(GTK_WINDOW(w));
	O.state = APP_HIDDEN;
	g_timeout_add_once(300, trim_idle, NULL);
	log_debug("overlay: hidden");
}

void overlay_set_state(AppState state, const char *detail)
{
	if (!O.win || state == APP_HIDDEN)
		return;
	for (guint i = 0; i < G_N_ELEMENTS(STATE_CLASS); i++)
		gtk_widget_remove_css_class(O.dot, STATE_CLASS[i]);
	gtk_widget_add_css_class(O.dot, STATE_CLASS[state]);
	O.state = state;

	char *text = NULL;
	switch (state) {
	case APP_CONNECTING: text = g_strdup("Connecting…"); break;
	case APP_RECONNECTING: text = g_strdup_printf("Reconnecting…%s%s", detail ? " " : "", detail ? detail : ""); break;
	case APP_ERROR: text = g_strdup_printf("Error%s%s", detail ? ": " : "", detail ? detail : ""); break;
	default: break; /* live / hidden: no text */
	}
	set_status_text(text);
	g_free(text);
}

void overlay_set_label(const char *label)
{
	if (O.name)
		gtk_label_set_text(GTK_LABEL(O.name), label);
}
