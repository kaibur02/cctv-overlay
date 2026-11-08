#define _GNU_SOURCE
#include "video.h"

#include "logging.h"
#include "overlay_gtk.h"
#include "util.h"

#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <gdk/gdk.h>
#include <gtk/gtk.h>
#include <mpv/client.h>
#include <mpv/render_gl.h>

#include <locale.h>
#include <malloc.h>
#include <stdint.h>
#include <string.h>

#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/wayland/gdkwayland.h>
#endif

/* libmpv render-API engine (M3).
 *
 * Threading: everything runs on the GLib/GTK main thread except two tiny foreign-thread callbacks
 * (mpv wakeup and render-update). Those only touch atomics and call g_idle_add(); all mpv calls
 * (events, render update, render) happen on the main thread, so there is no event thread to join.
 *
 * Lifetime: one fresh mpv handle + render context per session. hidden/stopped means no mpv handle,
 * no render context, no socket. Teardown order (proven in the M1 spike): free the render context
 * with the GL context current, then mpv_terminate_destroy().
 */

#define STALL_LIVE_MS 10000    /* no new frame while live */
#define STALL_CONNECT_MS 15000 /* no first frame after start (the RTSP socket timeout is 5 s) */
#define RTSP_TIMEOUT_US "5000000"
#define MAX_MPV_LOG_LINES 40

static struct {
	guint next_session;
	guint session; /* active session id, 0 = none */
	mpv_handle *mpv;
	mpv_render_context *rctx;
	GtkGLArea *area; /* weak pointer */
	gulong unrealize_id;
	VideoCallbacks cb;
	void *user;

	gint wake_queued;   /* atomic: a drain idle is pending */
	gint update_queued; /* atomic: a render-update idle is pending */
	gint frame_pending; /* atomic: a frame was signalled and not yet rendered */

	gboolean first_reported, outcome_reported;
	gboolean auth_failed, not_found, refused, timed_out;
	gint64 start_ms, last_frame_ms;
	guint watchdog;
	guint log_lines;
} V;

/* Every mpv/ffmpeg/Mesa worker thread gets its own glibc malloc arena, and arenas are kept after the thread
 * exits, so repeated show/hide cycles ratcheted RSS up by ~10 MB steps (measured: 68 -> 306 MB over 40
 * cycles; flat at 198 MB with MALLOC_ARENA_MAX=2). This is not a leak, but it defeats "hidden means
 * free". Cap the arenas before any thread exists (constructor), and trim when the overlay hides. */
__attribute__((constructor)) static void limit_malloc_arenas(void)
{
	mallopt(M_ARENA_MAX, 2);
}

/* ---------------- foreign-thread callbacks (atomics + g_idle_add only) ---------------- */

static gboolean drain_idle(gpointer data);
static gboolean update_idle(gpointer data);

static void on_wakeup(void *ctx)
{
	(void)ctx;
	if (g_atomic_int_compare_and_exchange(&V.wake_queued, 0, 1))
		g_idle_add(drain_idle, NULL);
}

static void on_render_update(void *ctx)
{
	(void)ctx;
	if (g_atomic_int_compare_and_exchange(&V.update_queued, 0, 1))
		g_idle_add(update_idle, NULL);
}

/* ---------------- reporting (main thread, from idle context only) ---------------- */

static void report_error(const char *msg)
{
	if (!V.session || V.outcome_reported)
		return;
	V.outcome_reported = TRUE;
	VideoCallbacks cb = V.cb; /* the callback normally calls video_stop(), which resets V */
	void *user = V.user;
	guint s = V.session;
	cb.error(user, s, msg);
}

static void report_ended(void)
{
	if (!V.session || V.outcome_reported)
		return;
	V.outcome_reported = TRUE;
	VideoCallbacks cb = V.cb;
	void *user = V.user;
	guint s = V.session;
	cb.ended(user, s);
}

static gboolean first_frame_idle(gpointer data)
{
	if (GPOINTER_TO_UINT(data) != V.session || V.first_reported)
		return G_SOURCE_REMOVE;
	V.first_reported = TRUE;
	log_debug("video: session %u first frame %" G_GINT64_FORMAT " ms after start", V.session,
		  util_monotonic_ms() - V.start_ms);
	VideoCallbacks cb = V.cb;
	void *user = V.user;
	guint s = V.session;
	cb.first_frame(user, s);
	return G_SOURCE_REMOVE;
}

static gboolean watchdog_cb(gpointer data)
{
	if (GPOINTER_TO_UINT(data) != V.session) {
		V.watchdog = 0;
		return G_SOURCE_REMOVE;
	}
	gint64 limit = V.first_reported ? STALL_LIVE_MS : STALL_CONNECT_MS;
	gint64 idle = util_monotonic_ms() - V.last_frame_ms;
	if (idle > limit) {
		V.watchdog = 0;
		report_error(V.first_reported ? "stream stalled (no video for 10 s)"
					      : "no video received (timed out)");
		return G_SOURCE_REMOVE;
	}
	return G_SOURCE_CONTINUE;
}

/* ---------------- render ---------------- */

static gboolean update_idle(gpointer data)
{
	(void)data;
	g_atomic_int_set(&V.update_queued, 0);
	if (!V.rctx)
		return G_SOURCE_REMOVE;
	if (mpv_render_context_update(V.rctx) & MPV_RENDER_UPDATE_FRAME) {
		g_atomic_int_set(&V.frame_pending, 1);
		V.last_frame_ms = util_monotonic_ms();
		if (V.area)
			gtk_gl_area_queue_render(V.area);
	}
	return G_SOURCE_REMOVE;
}

static void *egl_proc(void *ctx, const char *name)
{
	(void)ctx;
	return epoxy_eglGetProcAddress(name);
}

static gboolean render_hook(GtkGLArea *area, void *user)
{
	(void)user;
	GtkWidget *w = GTK_WIDGET(area);
	int scale = gtk_widget_get_scale_factor(w);
	int pw = gtk_widget_get_width(w) * scale, ph = gtk_widget_get_height(w) * scale;
	int fbo = 0;

	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
	if (!V.rctx || pw <= 0 || ph <= 0) {
		glClearColor(0, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		return TRUE;
	}

	mpv_opengl_fbo mfbo = { .fbo = fbo, .w = pw, .h = ph, .internal_format = 0 };
	int flip_y = 1, no_block = 0;
	mpv_render_param params[] = {
		{ MPV_RENDER_PARAM_OPENGL_FBO, &mfbo },
		{ MPV_RENDER_PARAM_FLIP_Y, &flip_y },
		{ MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &no_block }, /* never stall the GTK main loop */
		{ MPV_RENDER_PARAM_INVALID, NULL },
	};
	glViewport(0, 0, pw, ph);
	mpv_render_context_render(V.rctx, params);

	if (g_atomic_int_compare_and_exchange(&V.frame_pending, 1, 0) && !V.first_reported)
		g_idle_add(first_frame_idle, GUINT_TO_POINTER(V.session));
	return TRUE;
}

/* ---------------- events ---------------- */

static void handle_log(const mpv_event_log_message *m)
{
	if (m->log_level > MPV_LOG_LEVEL_WARN || V.log_lines > MAX_MPV_LOG_LINES)
		return;
	if (++V.log_lines > MAX_MPV_LOG_LINES) {
		log_warn("mpv: further messages suppressed for this session");
		return;
	}
	char *text = g_strdup(m->text ? m->text : "");
	g_strchomp(text);
	if (strstr(text, "401") || strstr(text, "Unauthorized"))
		V.auth_failed = TRUE;
	if (strstr(text, "404") || strstr(text, "Not Found"))
		V.not_found = TRUE;
	if (strstr(text, "Connection refused"))
		V.refused = TRUE;
	if (strstr(text, "timed out") || strstr(text, "Timed out") || strstr(text, "unreachable") ||
	    strstr(text, "No route"))
		V.timed_out = TRUE;
	char *safe = log_redact(text);
	if (m->log_level <= MPV_LOG_LEVEL_ERROR)
		log_error("mpv[%s]: %s", m->prefix ? m->prefix : "?", safe);
	else
		log_warn("mpv[%s]: %s", m->prefix ? m->prefix : "?", safe);
	g_free(safe);
	g_free(text);
}

static char *end_file_message(const mpv_event_end_file *ef)
{
	if (V.auth_failed)
		return g_strdup("authentication failed (check username and password)");
	if (V.not_found)
		return g_strdup("stream not found (check channel and path)");
	if (V.refused)
		return g_strdup("connection refused (is the recorder address and port right?)");
	if (V.timed_out)
		return g_strdup("recorder unreachable (connection timed out)");
	if (ef->error == MPV_ERROR_LOADING_FAILED || ef->error == MPV_ERROR_UNKNOWN_FORMAT)
		return g_strdup("could not open the stream (unreachable, refused or timed out)");
	return g_strdup_printf("playback failed: %s", mpv_error_string(ef->error));
}

static gboolean drain_idle(gpointer data)
{
	(void)data;
	g_atomic_int_set(&V.wake_queued, 0);

	while (V.mpv) {
		mpv_event *ev = mpv_wait_event(V.mpv, 0);
		if (ev->event_id == MPV_EVENT_NONE)
			break;

		switch (ev->event_id) {
		case MPV_EVENT_LOG_MESSAGE:
			handle_log(ev->data);
			break;
		case MPV_EVENT_FILE_LOADED:
			log_debug("video: session %u file loaded %" G_GINT64_FORMAT " ms after start", V.session,
				  util_monotonic_ms() - V.start_ms);
			break;
		case MPV_EVENT_PROPERTY_CHANGE: {
			mpv_event_property *p = ev->data;
			if (p->name && strcmp(p->name, "hwdec-current") == 0 && p->format == MPV_FORMAT_STRING &&
			    p->data && *(char **)p->data && **(char **)p->data)
				log_info("video: decoder '%s'", *(char **)p->data);
			break;
		}
		case MPV_EVENT_END_FILE: {
			mpv_event_end_file *ef = ev->data;
			if (ef->reason == MPV_END_FILE_REASON_ERROR) {
				char *m = end_file_message(ef);
				report_error(m); /* may stop the session: touch nothing of mpv afterwards */
				g_free(m);
				return G_SOURCE_REMOVE;
			}
			if (ef->reason == MPV_END_FILE_REASON_EOF) {
				report_ended();
				return G_SOURCE_REMOVE;
			}
			break;
		}
		default:
			break;
		}
	}
	return G_SOURCE_REMOVE;
}

/* ---------------- teardown ---------------- */

static void free_render_ctx(void)
{
	if (!V.rctx)
		return;
	mpv_render_context *c = V.rctx;
	V.rctx = NULL;
	gboolean cur = FALSE;
	if (V.area && gtk_widget_get_realized(GTK_WIDGET(V.area))) {
		gtk_gl_area_make_current(V.area);
		cur = gtk_gl_area_get_error(V.area) == NULL;
	}
	if (!cur)
		log_warn("video: freeing the render context without a current GL context");
	mpv_render_context_free(c);
}

static gboolean lost_idle(gpointer data)
{
	if (GPOINTER_TO_UINT(data) == V.session)
		report_error("overlay surface was lost");
	return G_SOURCE_REMOVE;
}

/* The overlay window is going away while a session is active (not via overlay_hide()). */
static void on_area_unrealize(GtkWidget *w, gpointer data)
{
	(void)w; (void)data;
	free_render_ctx();
	g_idle_add(lost_idle, GUINT_TO_POINTER(V.session));
}

void video_stop(void)
{
	if (!V.session && !V.mpv && !V.rctx)
		return;
	gint64 t0 = util_monotonic_ms();
	guint s = V.session;

	if (V.watchdog)
		g_source_remove(V.watchdog);
	V.watchdog = 0;
	if (V.area && V.unrealize_id)
		g_signal_handler_disconnect(V.area, V.unrealize_id);
	V.unrealize_id = 0;
	overlay_gtk_set_render_fn(NULL, NULL);

	free_render_ctx();
	if (V.mpv) {
		mpv_handle *h = V.mpv;
		V.mpv = NULL;
		mpv_set_wakeup_callback(h, NULL, NULL);
		mpv_terminate_destroy(h);
	}
	if (V.area) {
		gtk_gl_area_queue_render(V.area); /* repaint black */
		g_object_remove_weak_pointer(G_OBJECT(V.area), (gpointer *)&V.area);
		V.area = NULL;
	}
	V.session = 0;
	V.first_reported = V.outcome_reported = FALSE;
	log_debug("video: session %u stopped in %" G_GINT64_FORMAT " ms", s, util_monotonic_ms() - t0);
}

/* ---------------- start ---------------- */

static void set_opt(mpv_handle *h, const char *name, const char *value)
{
	int e = mpv_set_option_string(h, name, value);
	if (e < 0)
		log_warn("video: mpv option '%s' rejected: %s", name, mpv_error_string(e));
}

static void fail(GError **err, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void fail(GError **err, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	char *m = g_strdup_vprintf(fmt, ap);
	va_end(ap);
	g_set_error_literal(err, g_quark_from_static_string("cctv-video-error"), 1, m);
	g_free(m);
}

guint video_start(const Config *cfg, VideoUrlBuilder build, void *build_user, const VideoCallbacks *cb,
		  void *cb_user, GError **err)
{
	video_stop();

	char *url = build(build_user, err);
	if (!url)
		return 0;

	gint64 t0 = util_monotonic_ms();
	GtkGLArea *area = overlay_gtk_gl_area();
	if (!area) {
		util_wipe_free(url);
		fail(err, "no overlay window to render into");
		return 0;
	}
	if (!gtk_widget_get_realized(GTK_WIDGET(area)))
		gtk_widget_realize(GTK_WIDGET(area));
	gtk_gl_area_make_current(area);
	GError *gle = gtk_gl_area_get_error(area);
	if (gle) {
		util_wipe_free(url);
		fail(err, "OpenGL context unavailable: %s", gle->message);
		return 0;
	}

	/* GTK's gtk_init() sets the locale from the environment; libmpv refuses to start unless
	 * LC_NUMERIC is "C" (it would also mis-parse option values such as "0.2"). */
	setlocale(LC_NUMERIC, "C");
	mpv_handle *h = mpv_create();
	if (!h) {
		util_wipe_free(url);
		fail(err, "mpv_create failed");
		return 0;
	}
	set_opt(h, "config", "no");
	set_opt(h, "terminal", "no");
	set_opt(h, "load-scripts", "no");
	set_opt(h, "ytdl", "no");
	set_opt(h, "osc", "no");
	set_opt(h, "osd-level", "0");
	set_opt(h, "input-default-bindings", "no");
	set_opt(h, "input-vo-keyboard", "no");
	set_opt(h, "vo", "libmpv");
	set_opt(h, "profile", "low-latency");
	set_opt(h, "hwdec", cfg->hwdec);
	set_opt(h, "cache", "no");
	set_opt(h, "demuxer-readahead-secs", "0.2");
	set_opt(h, "audio", "no");
	set_opt(h, "ao", "null");
	set_opt(h, "sid", "no");
	set_opt(h, "rtsp-transport", cfg->transport);
	/* RTSP socket timeout in microseconds (NOT network-timeout, which RTSP ignores). The client API has no
	 * "-add" suffix, so this replaces the list the low-latency profile built: restate its fflags. */
	set_opt(h, "demuxer-lavf-o", "fflags=+nobuffer,timeout=" RTSP_TIMEOUT_US);
	set_opt(h, "keep-open", "no");
	set_opt(h, "idle", "yes");
	set_opt(h, "force-window", "no");

	int e = mpv_initialize(h);
	if (e < 0) {
		util_wipe_free(url);
		fail(err, "mpv_initialize failed: %s", mpv_error_string(e));
		mpv_terminate_destroy(h);
		return 0;
	}
	mpv_request_log_messages(h, "warn"); /* warn and above only; never forward verbose output */
	mpv_observe_property(h, 0, "hwdec-current", MPV_FORMAT_STRING);

	struct wl_display *wl = NULL;
#ifdef GDK_WINDOWING_WAYLAND
	GdkDisplay *gd = gdk_display_get_default();
	if (gd && GDK_IS_WAYLAND_DISPLAY(gd))
		wl = gdk_wayland_display_get_wl_display(gd); /* needed for VA-API <-> GL interop */
#endif
	mpv_opengl_init_params gl_init = { .get_proc_address = egl_proc };
	mpv_render_param rp[] = {
		{ MPV_RENDER_PARAM_API_TYPE, (void *)MPV_RENDER_API_TYPE_OPENGL },
		{ MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init },
		{ MPV_RENDER_PARAM_WL_DISPLAY, wl },
		{ MPV_RENDER_PARAM_INVALID, NULL },
	};
	if (!wl)
		rp[2].type = MPV_RENDER_PARAM_INVALID;
	mpv_render_context *rc = NULL;
	e = mpv_render_context_create(&rc, h, rp);
	if (e < 0) {
		util_wipe_free(url);
		fail(err, "mpv render context failed: %s", mpv_error_string(e));
		mpv_terminate_destroy(h);
		return 0;
	}

	V.mpv = h;
	V.rctx = rc;
	V.area = area;
	g_object_add_weak_pointer(G_OBJECT(area), (gpointer *)&V.area);
	V.unrealize_id = g_signal_connect(area, "unrealize", G_CALLBACK(on_area_unrealize), NULL);
	V.cb = *cb;
	V.user = cb_user;
	V.session = ++V.next_session;
	V.first_reported = V.outcome_reported = FALSE;
	V.auth_failed = V.not_found = V.refused = V.timed_out = FALSE;
	V.log_lines = 0;
	g_atomic_int_set(&V.frame_pending, 0);
	V.start_ms = t0;

	overlay_gtk_set_render_fn(render_hook, NULL);
	mpv_set_wakeup_callback(h, on_wakeup, NULL);
	mpv_render_context_set_update_callback(rc, on_render_update, NULL);

	/* The credentialed URL exists only for this call (mpv copies the arguments), then is wiped. */
	const char *cmd[] = { "loadfile", url, NULL };
	e = mpv_command_async(h, 0, cmd);
	util_wipe_free(url);
	if (e < 0) {
		fail(err, "could not start playback: %s", mpv_error_string(e));
		video_stop();
		return 0;
	}

	guint session = V.session;
	V.last_frame_ms = util_monotonic_ms();
	V.watchdog = g_timeout_add(1000, watchdog_cb, GUINT_TO_POINTER(session));
	log_debug("video: session %u started, setup took %" G_GINT64_FORMAT " ms (hwdec=%s, rtsp=%s)", session,
		  V.last_frame_ms - t0, cfg->hwdec, cfg->transport);
	return session;
}
