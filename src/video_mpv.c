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
 * Slots: up to VIDEO_MAX_SLOTS independent players, one per overlay tile (the grid view uses four).
 * Each has its own mpv handle, render context and GtkGLArea, so every tile decodes with zero-copy
 * VA-API and fails/reconnects on its own.
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
#define MAX_MPV_TRACE_LINES 20000

typedef struct {
	guint slot;
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
} Player;

static Player P[VIDEO_MAX_SLOTS];
static guint next_session;
static gboolean trace; /* CCTV_OVERLAY_MPV_TRACE: developer-only verbose mpv log (redacted) */

static Player *by_session(guint session)
{
	for (guint i = 0; session && i < VIDEO_MAX_SLOTS; i++)
		if (P[i].session == session)
			return &P[i];
	return NULL;
}

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
	Player *v = ctx;
	if (g_atomic_int_compare_and_exchange(&v->wake_queued, 0, 1))
		g_idle_add(drain_idle, v);
}

static void on_render_update(void *ctx)
{
	Player *v = ctx;
	if (g_atomic_int_compare_and_exchange(&v->update_queued, 0, 1))
		g_idle_add(update_idle, v);
}

/* ---------------- reporting (main thread, from idle context only) ---------------- */

static void report_error(Player *v, const char *msg)
{
	if (!v->session || v->outcome_reported)
		return;
	v->outcome_reported = TRUE;
	VideoCallbacks cb = v->cb; /* the callback normally calls video_stop(), which resets *v */
	void *user = v->user;
	guint s = v->session;
	cb.error(user, v->slot, s, msg);
}

static void report_ended(Player *v)
{
	if (!v->session || v->outcome_reported)
		return;
	v->outcome_reported = TRUE;
	VideoCallbacks cb = v->cb;
	void *user = v->user;
	guint s = v->session;
	cb.ended(user, v->slot, s);
}

static gboolean first_frame_idle(gpointer data)
{
	Player *v = by_session(GPOINTER_TO_UINT(data));
	if (!v || v->first_reported)
		return G_SOURCE_REMOVE;
	v->first_reported = TRUE;
	log_debug("video[%u]: session %u first frame %" G_GINT64_FORMAT " ms after start", v->slot, v->session,
		  util_monotonic_ms() - v->start_ms);
	VideoCallbacks cb = v->cb;
	void *user = v->user;
	guint s = v->session;
	cb.first_frame(user, v->slot, s);
	return G_SOURCE_REMOVE;
}

static gboolean watchdog_cb(gpointer data)
{
	Player *v = by_session(GPOINTER_TO_UINT(data));
	if (!v)
		return G_SOURCE_REMOVE; /* video_stop() already cleared the id */
	gint64 now = util_monotonic_ms();
	/* CONNECTING must time out from the start time, not from mpv update callbacks: a compositor that
	 * never gives us a frame callback would otherwise keep resetting last_frame_ms forever. */
	if (!v->first_reported && now - v->start_ms > STALL_CONNECT_MS) {
		v->watchdog = 0;
		report_error(v, "no video received (timed out)");
		return G_SOURCE_REMOVE;
	}
	if (v->first_reported && now - v->last_frame_ms > STALL_LIVE_MS) {
		v->watchdog = 0;
		report_error(v, "stream stalled (no video for 10 s)");
		return G_SOURCE_REMOVE;
	}
	return G_SOURCE_CONTINUE;
}

/* ---------------- render ---------------- */

static gboolean update_idle(gpointer data)
{
	Player *v = data;
	g_atomic_int_set(&v->update_queued, 0);
	if (!v->rctx)
		return G_SOURCE_REMOVE;
	if (mpv_render_context_update(v->rctx) & MPV_RENDER_UPDATE_FRAME) {
		g_atomic_int_set(&v->frame_pending, 1);
		v->last_frame_ms = util_monotonic_ms();
		if (v->area)
			gtk_gl_area_queue_render(v->area);
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
	Player *v = user;
	GtkWidget *w = GTK_WIDGET(area);
	int scale = gtk_widget_get_scale_factor(w);
	int pw = gtk_widget_get_width(w) * scale, ph = gtk_widget_get_height(w) * scale;
	int fbo = 0;

	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
	if (!v->rctx || pw <= 0 || ph <= 0) {
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
	mpv_render_context_render(v->rctx, params);

	if (g_atomic_int_compare_and_exchange(&v->frame_pending, 1, 0) && !v->first_reported)
		g_idle_add(first_frame_idle, GUINT_TO_POINTER(v->session));
	return TRUE;
}

/* ---------------- events ---------------- */

static void handle_log(Player *v, const mpv_event_log_message *m)
{
	if (trace) {
		/* Developer diagnostic (CCTV_OVERLAY_MPV_TRACE=1): verbose mpv lines with a time offset,
		 * always passed through the redactor. Not for normal use; mpv prints stream URLs at this level. */
		if (v->log_lines++ < MAX_MPV_TRACE_LINES) {
			char *t = g_strdup(m->text ? m->text : "");
			g_strchomp(t);
			char *s = log_redact(t);
			log_info("mpvtrace[%u] +%" G_GINT64_FORMAT "ms [%s] %s", v->slot, util_monotonic_ms() - v->start_ms,
				 m->prefix ? m->prefix : "?", s);
			g_free(s);
			g_free(t);
		}
		return;
	}
	if (m->log_level > MPV_LOG_LEVEL_WARN || v->log_lines > MAX_MPV_LOG_LINES)
		return;
	if (++v->log_lines > MAX_MPV_LOG_LINES) {
		log_warn("mpv[%u]: further messages suppressed for this session", v->slot);
		return;
	}
	char *text = g_strdup(m->text ? m->text : "");
	g_strchomp(text);
	if (strstr(text, "401") || strstr(text, "Unauthorized"))
		v->auth_failed = TRUE;
	if (strstr(text, "404") || strstr(text, "Not Found"))
		v->not_found = TRUE;
	if (strstr(text, "Connection refused"))
		v->refused = TRUE;
	if (strstr(text, "timed out") || strstr(text, "Timed out") || strstr(text, "unreachable") ||
	    strstr(text, "No route"))
		v->timed_out = TRUE;
	char *safe = log_redact(text);
	if (m->log_level <= MPV_LOG_LEVEL_ERROR)
		log_error("mpv[%u:%s]: %s", v->slot, m->prefix ? m->prefix : "?", safe);
	else
		log_warn("mpv[%u:%s]: %s", v->slot, m->prefix ? m->prefix : "?", safe);
	g_free(safe);
	g_free(text);
}

static char *end_file_message(Player *v, const mpv_event_end_file *ef)
{
	if (v->auth_failed)
		return g_strdup("authentication failed (check username and password)");
	if (v->not_found)
		return g_strdup("stream not found (check channel and path)");
	if (v->refused)
		return g_strdup("connection refused (is the recorder address and port right?)");
	if (v->timed_out)
		return g_strdup("recorder unreachable (connection timed out)");
	if (ef->error == MPV_ERROR_LOADING_FAILED || ef->error == MPV_ERROR_UNKNOWN_FORMAT)
		return g_strdup("could not open the stream (unreachable, refused or timed out)");
	return g_strdup_printf("playback failed: %s", mpv_error_string(ef->error));
}

static void check_audio_track(Player *v)
{
	gboolean has = FALSE;
	int64_t n = 0;
	if (mpv_get_property(v->mpv, "track-list/count", MPV_FORMAT_INT64, &n) < 0)
		return;
	for (int64_t i = 0; i < n && i < 64 && !has; i++) {
		char key[48];
		g_snprintf(key, sizeof key, "track-list/%" G_GINT64_FORMAT "/type", (gint64)i);
		char *type = mpv_get_property_string(v->mpv, key);
		has = type && strcmp(type, "audio") == 0;
		mpv_free(type);
	}
	log_info("video[%u]: stream %s an audio track", v->slot, has ? "has" : "has no");
	overlay_gtk_set_audio_track(v->slot, has);
}

static gboolean drain_idle(gpointer data)
{
	Player *v = data;
	g_atomic_int_set(&v->wake_queued, 0);

	while (v->mpv) {
		mpv_event *ev = mpv_wait_event(v->mpv, 0);
		if (ev->event_id == MPV_EVENT_NONE)
			break;

		switch (ev->event_id) {
		case MPV_EVENT_LOG_MESSAGE:
			handle_log(v, ev->data);
			break;
		case MPV_EVENT_FILE_LOADED:
			log_debug("video[%u]: session %u file loaded %" G_GINT64_FORMAT " ms after start", v->slot, v->session,
				  util_monotonic_ms() - v->start_ms);
			check_audio_track(v);
			break;
		case MPV_EVENT_PROPERTY_CHANGE: {
			mpv_event_property *p = ev->data;
			if (p->name && strcmp(p->name, "hwdec-current") == 0 && p->format == MPV_FORMAT_STRING &&
			    p->data && *(char **)p->data && **(char **)p->data)
				log_info("video[%u]: decoder '%s'", v->slot, *(char **)p->data);
			break;
		}
		case MPV_EVENT_SHUTDOWN:
			report_error(v, "video engine shut down");
			return G_SOURCE_REMOVE;
		case MPV_EVENT_END_FILE: {
			mpv_event_end_file *ef = ev->data;
			if (ef->reason == MPV_END_FILE_REASON_ERROR) {
				char *m = end_file_message(v, ef);
				report_error(v, m); /* may stop the session: touch nothing of mpv afterwards */
				g_free(m);
				return G_SOURCE_REMOVE;
			}
			if (ef->reason == MPV_END_FILE_REASON_EOF) {
				report_ended(v);
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

static void free_render_ctx(Player *v)
{
	if (!v->rctx)
		return;
	mpv_render_context *c = v->rctx;
	v->rctx = NULL;
	gboolean cur = FALSE;
	if (v->area && gtk_widget_get_realized(GTK_WIDGET(v->area))) {
		gtk_gl_area_make_current(v->area);
		cur = gtk_gl_area_get_error(v->area) == NULL;
	}
	if (!cur)
		log_warn("video[%u]: freeing the render context without a current GL context", v->slot);
	mpv_render_context_free(c);
}

static gboolean lost_idle(gpointer data)
{
	Player *v = by_session(GPOINTER_TO_UINT(data));
	if (v)
		report_error(v, "overlay surface was lost");
	return G_SOURCE_REMOVE;
}

/* The overlay tile is going away while a session is active (not via overlay_hide()). */
static void on_area_unrealize(GtkWidget *w, gpointer data)
{
	(void)w;
	Player *v = data;
	free_render_ctx(v);
	g_idle_add(lost_idle, GUINT_TO_POINTER(v->session));
}

void video_stop(guint slot)
{
	if (slot >= VIDEO_MAX_SLOTS)
		return;
	Player *v = &P[slot];
	if (!v->session && !v->mpv && !v->rctx)
		return;
	gint64 t0 = util_monotonic_ms();
	guint s = v->session;

	if (v->watchdog)
		g_source_remove(v->watchdog);
	v->watchdog = 0;
	if (v->area && v->unrealize_id)
		g_signal_handler_disconnect(v->area, v->unrealize_id);
	v->unrealize_id = 0;
	overlay_gtk_set_render_fn(slot, NULL, NULL);

	free_render_ctx(v);
	if (v->mpv) {
		mpv_handle *h = v->mpv;
		v->mpv = NULL;
		mpv_set_wakeup_callback(h, NULL, NULL);
		mpv_terminate_destroy(h);
	}
	if (v->area) {
		gtk_gl_area_queue_render(v->area); /* repaint black */
		g_object_remove_weak_pointer(G_OBJECT(v->area), (gpointer *)&v->area);
		v->area = NULL;
	}
	v->session = 0;
	v->first_reported = v->outcome_reported = FALSE;
	log_debug("video[%u]: session %u stopped in %" G_GINT64_FORMAT " ms", slot, s, util_monotonic_ms() - t0);
}

void video_stop_all(void)
{
	for (guint i = 0; i < VIDEO_MAX_SLOTS; i++)
		video_stop(i);
}

void video_set_audio(guint slot, gboolean on)
{
	if (slot >= VIDEO_MAX_SLOTS || !P[slot].mpv)
		return;
	int e = mpv_set_property_string(P[slot].mpv, "aid", on ? "auto" : "no");
	if (e < 0)
		log_warn("video[%u]: cannot switch audio %s: %s", slot, on ? "on" : "off", mpv_error_string(e));
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

guint video_start(guint slot, const Config *cfg, gboolean audio, VideoUrlBuilder build, void *build_user,
		  const VideoCallbacks *cb, void *cb_user, GError **err)
{
	if (slot >= VIDEO_MAX_SLOTS) {
		fail(err, "invalid video slot");
		return 0;
	}
	Player *v = &P[slot];
	video_stop(slot);

	char *url = build(build_user, slot, err);
	if (!url)
		return 0;

	gint64 t0 = util_monotonic_ms();
	GtkGLArea *area = overlay_gtk_gl_area(slot);
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
	/* Audio is opt-in (`cctv-overlay audio`): with aid=no mpv opens no audio output at all. The track is
	 * switched live with video_set_audio(). */
	set_opt(h, "aid", audio ? "auto" : "no");
	set_opt(h, "audio-client-name", "cctv-overlay");
	set_opt(h, "sid", "no");
	set_opt(h, "rtsp-transport", cfg->transport);
	/* RTSP socket timeout in microseconds (NOT network-timeout, which RTSP ignores). The client API has no
	 * "-add" suffix, so this replaces the list the low-latency profile built: restate its fflags. */
	set_opt(h, "demuxer-lavf-o", "fflags=+nobuffer,timeout=" RTSP_TIMEOUT_US);
	set_opt(h, "keep-open", "no");
	set_opt(h, "idle", "yes");
	set_opt(h, "force-window", "no");

	/* Developer-only tuning hook: CCTV_OVERLAY_MPV_OPTS="name=value;name=value" is applied last so latency
	 * experiments need no rebuild. Values are not logged (names only). Not documented for end users. */
	const char *dev = g_getenv("CCTV_OVERLAY_MPV_OPTS");
	if (dev && *dev) {
		gchar **pairs = g_strsplit(dev, ";", -1);
		for (gchar **p = pairs; p && *p; p++) {
			gchar *eq = strchr(*p, '=');
			if (!eq || eq == *p)
				continue;
			*eq = '\0';
			log_info("video: developer mpv option override '%s'", *p);
			set_opt(h, *p, eq + 1);
		}
		g_strfreev(pairs);
	}

	int e = mpv_initialize(h);
	if (e < 0) {
		util_wipe_free(url);
		fail(err, "mpv_initialize failed: %s", mpv_error_string(e));
		mpv_terminate_destroy(h);
		return 0;
	}
	const char *trace_env = g_getenv("CCTV_OVERLAY_MPV_TRACE");
	/* Only the exact values 1 or trace enable this; "0" and "" must not. */
	trace = trace_env && (strcmp(trace_env, "1") == 0 || strcmp(trace_env, "trace") == 0);
	/* warn and above only; verbose output is forwarded solely in the explicit developer trace mode
	 * (CCTV_OVERLAY_MPV_TRACE=1 -> "v", =trace -> "trace") */
	mpv_request_log_messages(h, trace ? (g_strcmp0(trace_env, "trace") == 0 ? "trace" : "v") : "warn");
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

	v->slot = slot;
	v->mpv = h;
	v->rctx = rc;
	v->area = area;
	g_object_add_weak_pointer(G_OBJECT(area), (gpointer *)&v->area);
	v->unrealize_id = g_signal_connect(area, "unrealize", G_CALLBACK(on_area_unrealize), v);
	v->cb = *cb;
	v->user = cb_user;
	v->session = ++next_session;
	v->first_reported = v->outcome_reported = FALSE;
	v->auth_failed = v->not_found = v->refused = v->timed_out = FALSE;
	v->log_lines = 0;
	g_atomic_int_set(&v->frame_pending, 0);
	v->start_ms = t0;

	overlay_gtk_set_render_fn(slot, render_hook, v);
	mpv_set_wakeup_callback(h, on_wakeup, v);
	mpv_render_context_set_update_callback(rc, on_render_update, v);

	/* The credentialed URL exists only for this call (mpv copies the arguments), then is wiped. */
	const char *cmd[] = { "loadfile", url, NULL };
	e = mpv_command_async(h, 0, cmd);
	util_wipe_free(url);
	if (e < 0) {
		fail(err, "could not start playback: %s", mpv_error_string(e));
		video_stop(slot);
		return 0;
	}

	guint session = v->session;
	v->last_frame_ms = util_monotonic_ms();
	v->watchdog = g_timeout_add(1000, watchdog_cb, GUINT_TO_POINTER(session));
	log_debug("video[%u]: session %u started, setup took %" G_GINT64_FORMAT " ms (hwdec=%s, rtsp=%s, audio=%s)",
		  slot, session, v->last_frame_ms - t0, cfg->hwdec, cfg->transport, audio ? "on" : "off");
	return session;
}
