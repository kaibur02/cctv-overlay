#define _GNU_SOURCE
#include "app.h"

#include "overlay.h"
#include "util.h"
#include "video.h"

#include <string.h>

#define APP_ERROR_QUARK (g_quark_from_static_string("cctv-app-error"))

struct App {
	Config *cfg;
	CameraList *cams;
	AppOptions opt;
	AppState state;
	guint session;
	guint retries;
	guint retry_src;
	guint live_src;      /* resets the backoff only after a sustained LIVE period */
	guint stable_ms;
	char *error;
	gint64 start_ms;
};

static const guint BACKOFF_MS[APP_MAX_RETRIES] = { 500, 1000, 2000, 4000, 8000 };

static void app_fail(GError **err, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void app_fail(GError **err, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	char *m = g_strdup_vprintf(fmt, ap);
	va_end(ap);
	g_set_error_literal(err, APP_ERROR_QUARK, 1, m);
	g_free(m);
}

/* ---- accessors ---- */

AppState app_state(const App *a) { return a->state; }
const Config *app_config(const App *a) { return a->cfg; }
const Camera *app_current_camera(const App *a) { return camera_list_current_camera(a->cams); }
guint app_current_index(const App *a) { return camera_list_current(a->cams); }
guint app_retry_count(const App *a) { return a->retries; }
const char *app_last_error(const App *a) { return a->error; }

/* ---- internals ---- */

static void set_error(App *a, const char *msg)
{
	g_free(a->error);
	a->error = msg ? log_redact(msg) : NULL;
}

static void set_state(App *a, AppState s)
{
	if (a->state == s)
		return;
	log_info("state %s -> %s (camera '%s')", app_state_name(a->state), app_state_name(s),
		 app_current_camera(a)->name);
	a->state = s;
	if (a->opt.overlay_enabled && s != APP_HIDDEN)
		overlay_set_state(s, a->error);
}

static char *build_url(void *user, GError **err)
{
	App *a = user;
	return camera_resolve_url(a->cfg, app_current_camera(a), err);
}

static void on_first_frame(void *user, guint session);
static void on_error(void *user, guint session, const char *msg);
static void on_ended(void *user, guint session);
static const VideoCallbacks VCB = { on_first_frame, on_error, on_ended };

static void stop_stream(App *a)
{
	if (a->retry_src) {
		g_source_remove(a->retry_src);
		a->retry_src = 0;
	}
	if (a->live_src) {
		g_source_remove(a->live_src);
		a->live_src = 0;
	}
	video_stop();
	a->session = 0;
}

/* Begin a connection attempt for the current camera (state must already allow it). */
static void start_stream(App *a)
{
	GError *e = NULL;

	stop_stream(a);
	a->session = video_start(a->cfg, build_url, a, &VCB, a, &e);
	if (!a->session) {
		/* Configuration-class failure (e.g. missing password): retrying cannot help. */
		set_error(a, e ? e->message : "could not start the video stream");
		g_clear_error(&e);
		log_warn("cannot start stream for '%s': %s", app_current_camera(a)->name, a->error);
		set_state(a, APP_ERROR);
		return;
	}
	set_state(a, APP_CONNECTING);
}

static gboolean retry_cb(gpointer data)
{
	App *a = data;
	a->retry_src = 0;
	if (a->state == APP_RECONNECTING)
		start_stream(a);
	return G_SOURCE_REMOVE;
}

static void stream_failed(App *a, const char *msg)
{
	stop_stream(a); /* no decode and no network while backing off */
	set_error(a, msg);
	log_warn("stream problem on '%s': %s", app_current_camera(a)->name, a->error);

	if (a->cfg->reconnect && a->retries < APP_MAX_RETRIES) {
		guint ms = (guint)(BACKOFF_MS[a->retries] * a->opt.backoff_scale);
		a->retries++;
		set_state(a, APP_RECONNECTING);
		log_info("retry %u/%d in %u ms", a->retries, APP_MAX_RETRIES, ms);
		a->retry_src = g_timeout_add(ms ? ms : 1, retry_cb, a);
	} else {
		set_state(a, APP_ERROR);
	}
}

static gboolean live_stable_cb(gpointer data)
{
	App *a = data;
	a->live_src = 0;
	if (a->state == APP_LIVE) {
		a->retries = 0;
		log_debug("stream stable; reconnect backoff reset");
	}
	return G_SOURCE_REMOVE;
}

static void on_first_frame(void *user, guint session)
{
	App *a = user;
	if (session != a->session || a->state != APP_CONNECTING)
		return;
	set_error(a, NULL);
	set_state(a, APP_LIVE);
	/* Do not reset the backoff on the first frame: a stream that delivers one frame and drops would
	 * otherwise restart at 0.5 s forever. Reset only after it has stayed live for stable_ms. */
	if (a->stable_ms == 0)
		a->retries = 0;
	else if (a->retries > 0 && !a->live_src)
		a->live_src = g_timeout_add(a->stable_ms, live_stable_cb, a);
}

static void on_error(void *user, guint session, const char *msg)
{
	App *a = user;
	if (session != a->session || (a->state != APP_CONNECTING && a->state != APP_LIVE))
		return;
	stream_failed(a, msg ? msg : "stream error");
}

static void on_ended(void *user, guint session)
{
	on_error(user, session, "stream ended");
}

/* Open or (re)connect the current camera as a user action: resets the backoff. */
static void open_current(App *a)
{
	const char *name = app_current_camera(a)->name;

	a->retries = 0;
	set_error(a, NULL);
	a->start_ms = util_monotonic_ms();
	/* Idempotent: re-shows a window that exists, and re-creates one that was destroyed behind our
	 * back (monitor unplug, compositor closed the layer surface). Without this, the app could get
	 * stuck failing to start a stream because there is no GL area. */
	if (a->opt.overlay_enabled)
		overlay_show(a->cfg, name);
	start_stream(a); /* sets CONNECTING, or ERROR when the stream cannot even start */
}

/* ---- public ---- */

App *app_new(Config *cfg, const AppOptions *opt)
{
	App *a = g_new0(App, 1);
	a->cfg = cfg;
	a->cams = camera_list_new(cfg);
	a->opt = *opt;
	if (!(a->opt.backoff_scale > 0) || a->opt.backoff_scale > 600)
		a->opt.backoff_scale = 1.0;
	/* Test knob; 0 restores the old "reset on first frame" behaviour. */
	const char *stable = g_getenv("CCTV_OVERLAY_LIVE_STABLE_MS");
	a->stable_ms = 15000;
	if (stable && *stable) {
		gint64 v = g_ascii_strtoll(stable, NULL, 10);
		a->stable_ms = (guint)CLAMP(v, 0, 600000);
	}
	a->state = APP_HIDDEN;
	a->start_ms = util_monotonic_ms();
	log_set_level(cfg->log_level);
	return a;
}

void app_hide(App *a)
{
	if (a->state == APP_HIDDEN)
		return;
	stop_stream(a);
	a->retries = 0;
	set_error(a, NULL);
	set_state(a, APP_HIDDEN);
	if (a->opt.overlay_enabled)
		overlay_hide();
}

void app_free(App *a)
{
	if (!a)
		return;
	app_hide(a);
	camera_list_free(a->cams);
	config_free(a->cfg);
	g_free(a->error);
	g_free(a);
}

static void select_and_open(App *a, gboolean changed)
{
	switch (a->state) {
	case APP_HIDDEN:
	case APP_ERROR:
	case APP_RECONNECTING:
		open_current(a);
		break;
	case APP_CONNECTING:
	case APP_LIVE:
		if (changed)
			open_current(a);
		break;
	}
}

gboolean app_toggle(App *a, GError **err)
{
	(void)err;
	if (a->state == APP_HIDDEN || a->state == APP_ERROR)
		open_current(a);
	else
		app_hide(a);
	return TRUE;
}

gboolean app_show(App *a, const char *spec, GError **err)
{
	gboolean changed = FALSE;
	guint idx;

	if (spec && *spec) {
		if (!camera_list_find(a->cams, spec, &idx)) {
			app_fail(err, "unknown camera '%s' (see 'cctv-overlay list')", spec);
			return FALSE;
		}
		changed = idx != camera_list_current(a->cams);
		camera_list_set_current(a->cams, idx);
	}
	select_and_open(a, changed);
	return TRUE;
}

gboolean app_next(App *a, GError **err)
{
	(void)err;
	guint before = camera_list_current(a->cams);
	camera_list_next(a->cams);
	select_and_open(a, before != camera_list_current(a->cams));
	return TRUE;
}

gboolean app_prev(App *a, GError **err)
{
	(void)err;
	guint before = camera_list_current(a->cams);
	camera_list_prev(a->cams);
	select_and_open(a, before != camera_list_current(a->cams));
	return TRUE;
}

gboolean app_reload(App *a, GError **err)
{
	Config *nc = config_load(a->cfg->config_path, err);
	if (!nc)
		return FALSE;

	const Camera *cur = app_current_camera(a);
	char *cur_id = g_strdup(cur->id);
	guint idx = 0;
	gboolean found;

	CameraList *nl = camera_list_new(nc);
	found = camera_list_find_id(nl, cur_id, &idx);
	camera_list_set_current(nl, found ? idx : 0);

	Config *old = a->cfg;
	CameraList *oldl = a->cams;
	a->cfg = nc;
	a->cams = nl;
	camera_list_free(oldl);
	config_free(old);
	g_free(cur_id);
	log_set_level(nc->log_level);
	log_info("configuration reloaded (%u cameras)", nc->cameras->len);

	if (a->state == APP_ERROR || a->state == APP_RECONNECTING) {
		open_current(a); /* e.g. the password was just fixed: retry now */
	} else if (a->state != APP_HIDDEN) {
		if (!found)
			open_current(a); /* previous camera is gone: restart on the first one */
		else if (a->opt.overlay_enabled)
			overlay_set_label(app_current_camera(a)->name);
	}
	return TRUE;
}

void app_request_quit(App *a)
{
	if (a->opt.quit_cb)
		a->opt.quit_cb(a->opt.quit_user);
}

/* ---- IPC glue ---- */

static JsonObject *status_object(App *a)
{
	JsonObject *o = json_object_new();
	const Camera *c = app_current_camera(a);

	json_object_set_boolean_member(o, "ok", TRUE);
	json_object_set_boolean_member(o, "visible", a->state != APP_HIDDEN);
	json_object_set_string_member(o, "state", app_state_name(a->state));
	json_object_set_string_member(o, "camera", c->name);
	json_object_set_int_member(o, "camera_index", camera_list_current(a->cams));
	json_object_set_int_member(o, "camera_count", camera_list_count(a->cams));
	if (a->error)
		json_object_set_string_member(o, "error", a->error);
	else
		json_object_set_null_member(o, "error");
	json_object_set_int_member(o, "retries", a->retries);
	json_object_set_int_member(o, "uptime_ms", util_monotonic_ms() - a->start_ms);
	return o;
}

static JsonObject *list_object(App *a)
{
	JsonObject *o = json_object_new();
	JsonArray *arr = json_array_new();

	for (guint i = 0; i < camera_list_count(a->cams); i++) {
		const Camera *c = camera_list_get(a->cams, i);
		JsonObject *co = json_object_new();
		json_object_set_int_member(co, "index", i);
		json_object_set_string_member(co, "id", c->id);
		json_object_set_string_member(co, "name", c->name);
		json_object_set_int_member(co, "channel", c->channel);
		json_object_set_boolean_member(co, "current", i == camera_list_current(a->cams));
		json_array_add_object_element(arr, co);
	}
	json_object_set_boolean_member(o, "ok", TRUE);
	json_object_set_array_member(o, "cameras", arr);
	return o;
}

JsonObject *app_ipc_handler(void *user, const char *cmd, JsonObject *req)
{
	App *a = user;
	GError *e = NULL;
	gboolean ok = TRUE;

	if (strcmp(cmd, "ping") == 0) {
		JsonObject *o = json_object_new();
		json_object_set_boolean_member(o, "ok", TRUE);
		return o;
	} else if (strcmp(cmd, "status") == 0) {
		return status_object(a);
	} else if (strcmp(cmd, "list") == 0) {
		return list_object(a);
	} else if (strcmp(cmd, "toggle") == 0) {
		ok = app_toggle(a, &e);
	} else if (strcmp(cmd, "next") == 0) {
		ok = app_next(a, &e);
	} else if (strcmp(cmd, "prev") == 0) {
		ok = app_prev(a, &e);
	} else if (strcmp(cmd, "hide") == 0) {
		app_hide(a);
	} else if (strcmp(cmd, "show") == 0) {
		const char *spec = NULL;
		JsonNode *n = json_object_get_member(req, "camera");
		if (n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING)
			spec = json_node_get_string(n);
		ok = app_show(a, spec, &e);
	} else if (strcmp(cmd, "reload") == 0) {
		ok = app_reload(a, &e);
	} else if (strcmp(cmd, "quit") == 0) {
		/* the loop is stopped by app_ipc_after_reply() once the reply is written */
	} else {
		JsonObject *o = json_object_new();
		json_object_set_boolean_member(o, "ok", FALSE);
		char *m = g_strdup_printf("unknown command '%.32s'", cmd);
		json_object_set_string_member(o, "error", m);
		g_free(m);
		return o;
	}

	if (!ok) {
		JsonObject *o = json_object_new();
		json_object_set_boolean_member(o, "ok", FALSE);
		json_object_set_string_member(o, "error", e ? e->message : "command failed");
		g_clear_error(&e);
		return o;
	}
	return status_object(a);
}

void app_ipc_after_reply(void *user, const char *cmd)
{
	if (strcmp(cmd, "quit") == 0)
		app_request_quit(user);
}
