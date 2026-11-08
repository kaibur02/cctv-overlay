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

#define MAX_TILES 4

typedef struct {
	GtkWidget *box, *gl, *chip, *dot, *name, *status;
	AppState state;
	OverlayRenderFn render_fn;
	void *render_user;
	gboolean audio_track;
} OTile;

static struct {
	gboolean inited;
	GtkWidget *win, *frame, *grid, *speaker;
	gboolean show_label, show_status;
	guint ntiles;
	OTile t[MAX_TILES];
	gboolean audio, audio_dimmed;
	guint trim_src;
} O;

static const char *const STATE_CLASS[] = { "hidden", "connecting", "live", "reconnecting", "error" };

static const char CSS[] =
	"window.cctv-overlay { background: transparent; }\n"
	".cctv-frame { background: #000; border-radius: 8px; }\n"
	".cctv-grid { background: #111; }\n"
	".cctv-tile { background: #000; }\n"
	".cctv-chip { background: rgba(0,0,0,0.55); border-radius: 6px; padding: 2px 8px; margin: 6px; }\n"
	".cctv-tiled .cctv-chip { padding: 1px 6px; margin: 4px; }\n"
	".cctv-name { color: #fff; font-size: 12px; font-weight: 600; }\n"
	".cctv-tiled .cctv-name { font-size: 10px; }\n"
	".cctv-dot { min-width: 9px; min-height: 9px; border-radius: 5px; background: #888; }\n"
	".cctv-tiled .cctv-dot { min-width: 7px; min-height: 7px; border-radius: 4px; }\n"
	".cctv-dot.connecting, .cctv-dot.reconnecting { background: #f0a500; }\n"
	".cctv-dot.live { background: #2ecc71; }\n"
	".cctv-dot.error { background: #e74c3c; }\n"
	".cctv-status { color: #fff; background: rgba(0,0,0,0.6); border-radius: 6px; padding: 2px 8px;"
	" margin: 8px; font-size: 11px; }\n"
	".cctv-tiled .cctv-status { font-size: 9px; margin: 4px; padding: 1px 6px; }\n"
	".cctv-speaker { background: rgba(0,0,0,0.55); border-radius: 6px; margin: 6px; padding: 3px; }\n";

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

GtkGLArea *overlay_gtk_gl_area(guint slot)
{
	return slot < O.ntiles && O.t[slot].gl ? GTK_GL_AREA(O.t[slot].gl) : NULL;
}

void overlay_gtk_set_render_fn(guint slot, OverlayRenderFn fn, void *user)
{
	if (slot >= MAX_TILES)
		return;
	O.t[slot].render_fn = fn;
	O.t[slot].render_user = user;
}

static void forget_widgets(void)
{
	O.win = O.frame = O.grid = O.speaker = NULL;
	for (guint i = 0; i < MAX_TILES; i++)
		O.t[i].box = O.t[i].gl = O.t[i].chip = O.t[i].dot = O.t[i].name = O.t[i].status = NULL;
	O.ntiles = 0;
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
		forget_widgets();
	}
}

static gboolean on_gl_render(GtkGLArea *area, GdkGLContext *ctx, gpointer data)
{
	(void)ctx;
	OTile *t = data;
	if (t->render_fn)
		return t->render_fn(area, t->render_user);
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

static void set_status_text(OTile *t, const char *text)
{
	if (!t->status)
		return;
	gboolean show = text && *text && O.show_status;
	gtk_label_set_text(GTK_LABEL(t->status), show ? text : "");
	gtk_widget_set_visible(t->status, show);
}

static void refresh_chip(OTile *t)
{
	if (!t->chip)
		return;
	gtk_widget_set_visible(t->dot, O.show_status);
	gtk_widget_set_visible(t->name, O.show_label);
	gtk_widget_set_visible(t->chip, O.show_label || O.show_status);
}

/* ---------------- speaker icon (drawn, so no icon theme is needed) ---------------- */

static void draw_speaker(GtkDrawingArea *da, cairo_t *cr, int w, int h, gpointer data)
{
	(void)da; (void)data;
	double s = MIN(w, h) / 16.0;
	gboolean crossed = O.ntiles == 1 && !O.t[0].audio_track;
	double alpha = O.audio_dimmed ? 0.45 : 1.0;

	cairo_scale(cr, s, s);
	cairo_set_source_rgba(cr, 1, 1, 1, alpha);
	/* body + cone */
	cairo_move_to(cr, 1.5, 5.5);
	cairo_line_to(cr, 4.5, 5.5);
	cairo_line_to(cr, 8.5, 2);
	cairo_line_to(cr, 8.5, 14);
	cairo_line_to(cr, 4.5, 10.5);
	cairo_line_to(cr, 1.5, 10.5);
	cairo_close_path(cr);
	cairo_fill(cr);
	cairo_set_line_width(cr, 1.4);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	if (crossed || O.audio_dimmed) { /* no audio track / grid view: an "x" instead of sound waves */
		cairo_move_to(cr, 10.5, 5.5);
		cairo_line_to(cr, 14.5, 10.5);
		cairo_move_to(cr, 14.5, 5.5);
		cairo_line_to(cr, 10.5, 10.5);
		cairo_stroke(cr);
		return;
	}
	cairo_arc(cr, 8.5, 8, 3.2, -G_PI / 4, G_PI / 4);
	cairo_stroke(cr);
	cairo_arc(cr, 8.5, 8, 6.0, -G_PI / 3.2, G_PI / 3.2);
	cairo_stroke(cr);
}

static void refresh_speaker(void)
{
	if (!O.speaker)
		return;
	gtk_widget_set_visible(O.speaker, O.audio);
	gtk_widget_queue_draw(O.speaker);
}

void overlay_set_audio(gboolean enabled, gboolean muted_view)
{
	O.audio = enabled;
	O.audio_dimmed = muted_view;
	refresh_speaker();
}

void overlay_gtk_set_audio_track(guint slot, gboolean present)
{
	if (slot < MAX_TILES && O.t[slot].audio_track != present) {
		O.t[slot].audio_track = present;
		refresh_speaker();
	}
}

/* ---------------- tiles ---------------- */

static void build_tile(OTile *t, const char *label)
{
	t->box = gtk_overlay_new();
	gtk_widget_add_css_class(t->box, "cctv-tile");
	gtk_widget_set_overflow(t->box, GTK_OVERFLOW_HIDDEN);
	gtk_widget_set_hexpand(t->box, TRUE);
	gtk_widget_set_vexpand(t->box, TRUE);

	t->gl = gtk_gl_area_new();
	gtk_gl_area_set_has_depth_buffer(GTK_GL_AREA(t->gl), FALSE);
	gtk_gl_area_set_has_stencil_buffer(GTK_GL_AREA(t->gl), FALSE);
	gtk_gl_area_set_allowed_apis(GTK_GL_AREA(t->gl), GDK_GL_API_GL | GDK_GL_API_GLES);
	gtk_widget_set_hexpand(t->gl, TRUE);
	gtk_widget_set_vexpand(t->gl, TRUE);
	g_signal_connect(t->gl, "render", G_CALLBACK(on_gl_render), t);
	gtk_overlay_set_child(GTK_OVERLAY(t->box), t->gl);

	t->chip = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_add_css_class(t->chip, "cctv-chip");
	gtk_widget_set_halign(t->chip, GTK_ALIGN_START);
	gtk_widget_set_valign(t->chip, GTK_ALIGN_START);
	t->dot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_add_css_class(t->dot, "cctv-dot");
	gtk_widget_set_valign(t->dot, GTK_ALIGN_CENTER);
	t->name = gtk_label_new(label);
	gtk_widget_add_css_class(t->name, "cctv-name");
	gtk_label_set_ellipsize(GTK_LABEL(t->name), PANGO_ELLIPSIZE_END);
	gtk_label_set_max_width_chars(GTK_LABEL(t->name), 32);
	gtk_box_append(GTK_BOX(t->chip), t->dot);
	gtk_box_append(GTK_BOX(t->chip), t->name);
	gtk_overlay_add_overlay(GTK_OVERLAY(t->box), t->chip);

	t->status = gtk_label_new("");
	gtk_widget_add_css_class(t->status, "cctv-status");
	gtk_widget_set_halign(t->status, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(t->status, GTK_ALIGN_END);
	gtk_label_set_ellipsize(GTK_LABEL(t->status), PANGO_ELLIPSIZE_END);
	gtk_label_set_max_width_chars(GTK_LABEL(t->status), 48);
	gtk_overlay_add_overlay(GTK_OVERLAY(t->box), t->status);

	/* Nothing in the overlay takes pointer input, in addition to the empty input region. */
	GtkWidget *w[] = { t->box, t->gl, t->chip, t->dot, t->name, t->status };
	for (guint i = 0; i < G_N_ELEMENTS(w); i++) {
		gtk_widget_set_can_target(w[i], FALSE);
		gtk_widget_set_focusable(w[i], FALSE);
	}

	t->state = APP_CONNECTING;
	t->audio_track = TRUE; /* until the engine says otherwise */
	gtk_widget_add_css_class(t->dot, "connecting");
	set_status_text(t, "Connecting…");
	refresh_chip(t);
}

/* (Re)build the tile grid inside the frame. The caller has stopped every video slot. */
static void build_tiles(guint n, const char *const *labels)
{
	if (O.grid) /* unparenting destroys the old tiles and their GL areas */
		gtk_overlay_set_child(GTK_OVERLAY(O.frame), NULL);
	for (guint i = 0; i < MAX_TILES; i++)
		O.t[i].box = O.t[i].gl = O.t[i].chip = O.t[i].dot = O.t[i].name = O.t[i].status = NULL;

	O.grid = gtk_grid_new();
	gtk_widget_add_css_class(O.grid, "cctv-grid");
	gtk_grid_set_row_homogeneous(GTK_GRID(O.grid), TRUE);
	gtk_grid_set_column_homogeneous(GTK_GRID(O.grid), TRUE);
	gtk_widget_set_can_target(O.grid, FALSE);
	if (n > 1) {
		gtk_grid_set_row_spacing(GTK_GRID(O.grid), 2);
		gtk_grid_set_column_spacing(GTK_GRID(O.grid), 2);
		gtk_widget_add_css_class(O.frame, "cctv-tiled");
	} else {
		gtk_widget_remove_css_class(O.frame, "cctv-tiled");
	}
	for (guint i = 0; i < n; i++) {
		build_tile(&O.t[i], labels[i] ? labels[i] : "");
		/* 1 tile fills the frame; 2..4 tiles use a 2x2 layout (empty cells stay black) */
		gtk_grid_attach(GTK_GRID(O.grid), O.t[i].box, n > 1 ? (int)(i % 2) : 0, n > 1 ? (int)(i / 2) : 0, 1, 1);
	}
	if (n == 2 || n == 3) { /* keep the 2x2 cell size: fill the remaining cells with black */
		for (guint i = n; i < 4; i++) {
			GtkWidget *pad = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
			gtk_widget_add_css_class(pad, "cctv-tile");
			gtk_widget_set_hexpand(pad, TRUE);
			gtk_widget_set_vexpand(pad, TRUE);
			gtk_widget_set_can_target(pad, FALSE);
			gtk_grid_attach(GTK_GRID(O.grid), pad, (int)(i % 2), (int)(i / 2), 1, 1);
		}
	}
	O.ntiles = n;
	gtk_overlay_set_child(GTK_OVERLAY(O.frame), O.grid);
	if (O.win && gtk_widget_get_realized(O.win))
		for (guint i = 0; i < n; i++)
			gtk_widget_realize(O.t[i].gl);
}

/* ---------------- interface ---------------- */

void overlay_show(const Config *cfg, guint ntiles, const char *const *labels)
{
	ntiles = CLAMP(ntiles, 1, MAX_TILES);
	if (O.trim_src) { /* cancel a pending malloc_trim from a recent hide */
		g_source_remove(O.trim_src);
		O.trim_src = 0;
	}
	if (O.win) {
		if (ntiles != O.ntiles) {
			build_tiles(ntiles, labels);
		} else {
			for (guint i = 0; i < ntiles; i++)
				if (O.t[i].name)
					gtk_label_set_text(GTK_LABEL(O.t[i].name), labels[i] ? labels[i] : "");
		}
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

	/* Frame: the tile grid (video), plus the speaker icon in the top-right corner. */
	O.frame = gtk_overlay_new();
	gtk_widget_add_css_class(O.frame, "cctv-frame");
	gtk_widget_set_overflow(O.frame, GTK_OVERFLOW_HIDDEN); /* clips the video to the rounded corners */
	gtk_widget_set_can_target(O.frame, FALSE);
	gtk_widget_set_focusable(O.frame, FALSE);

	O.speaker = gtk_drawing_area_new();
	gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(O.speaker), 16);
	gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(O.speaker), 16);
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(O.speaker), draw_speaker, NULL, NULL);
	gtk_widget_add_css_class(O.speaker, "cctv-speaker");
	gtk_widget_set_halign(O.speaker, GTK_ALIGN_END);
	gtk_widget_set_valign(O.speaker, GTK_ALIGN_START);
	gtk_widget_set_can_target(O.speaker, FALSE);
	gtk_widget_set_focusable(O.speaker, FALSE);
	gtk_overlay_add_overlay(GTK_OVERLAY(O.frame), O.speaker);

	build_tiles(ntiles, labels);
	refresh_speaker();
	gtk_window_set_child(win, O.frame);

	g_signal_connect(win, "destroy", G_CALLBACK(on_win_destroy), NULL);
	g_signal_connect_after(win, "realize", G_CALLBACK(on_win_realize), NULL);
	g_signal_connect_after(win, "map", G_CALLBACK(on_win_map), NULL);

	/* gtk_widget_set_visible() maps without the activation/focus request of gtk_window_present(). */
	gtk_widget_realize(GTK_WIDGET(win));
	for (guint i = 0; i < O.ntiles; i++)
		gtk_widget_realize(O.t[i].gl);
	gtk_widget_set_visible(GTK_WIDGET(win), TRUE);
	log_debug("overlay: shown %dx%d tiles=%u anchor=%s monitor=%s layer=%s", cfg->width, cfg->height, ntiles,
		  cfg->anchor, cfg->monitor, cfg->layer);
}

static void trim_idle(gpointer data)
{
	(void)data;
	O.trim_src = 0;
	malloc_trim(0); /* hand freed pages (mpv, GL driver, GTK) back while the overlay is hidden */
}

void overlay_hide(void)
{
	if (!O.win)
		return;
	video_stop_all(); /* render contexts must die while their GL contexts are alive */
	GtkWidget *w = O.win;
	forget_widgets();
	gtk_window_destroy(GTK_WINDOW(w));
	if (O.trim_src)
		g_source_remove(O.trim_src);
	O.trim_src = g_timeout_add_once(300, trim_idle, NULL);
	log_debug("overlay: hidden");
}

void overlay_set_state(guint tile, AppState state, const char *detail)
{
	if (!O.win || state == APP_HIDDEN || tile >= O.ntiles || !O.t[tile].dot)
		return;
	OTile *t = &O.t[tile];
	for (guint i = 0; i < G_N_ELEMENTS(STATE_CLASS); i++)
		gtk_widget_remove_css_class(t->dot, STATE_CLASS[i]);
	gtk_widget_add_css_class(t->dot, STATE_CLASS[state]);
	t->state = state;

	char *text = NULL;
	switch (state) {
	case APP_CONNECTING: text = g_strdup("Connecting…"); break;
	case APP_RECONNECTING: text = g_strdup_printf("Reconnecting…%s%s", detail ? " " : "", detail ? detail : ""); break;
	case APP_ERROR: text = g_strdup_printf("Error%s%s", detail ? ": " : "", detail ? detail : ""); break;
	default: break; /* live / hidden: no text */
	}
	set_status_text(t, text);
	g_free(text);
}
