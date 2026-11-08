#define _GNU_SOURCE
#include "app.h"

#include "overlay.h"
#include "util.h"
#include "video.h"

#include <string.h>

#define APP_ERROR_QUARK (g_quark_from_static_string("cctv-app-error"))

/* One video tile. The single-camera view uses tile 0 only; the grid view uses tiles 0..n-1. Each tile
 * has its own session, state and reconnect backoff, so one offline camera never takes the grid down. */
typedef struct {
	App *app;
	guint slot;
	guint cam; /* camera index */
	AppState state;
	guint session;
	guint retries;
	guint retry_src;
	guint live_src; /* resets the backoff only after a sustained LIVE period */
	char *error;
} Tile;

struct App {
	Config *cfg;
	CameraList *cams;
	AppOptions opt;
	gboolean visible;
	gboolean grid;  /* the selected view is the grid */
	guint ntiles;   /* tiles in use while visible */
	Tile t[VIDEO_MAX_SLOTS];
	AppState state; /* cached aggregate, for logging */
	gboolean audio;
	guint stable_ms;
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
gboolean app_grid_view(const App *a) { return a->grid; }
gboolean app_audio(const App *a) { return a->audio; }
const Config *app_config(const App *a) { return a->cfg; }
const Camera *app_current_camera(const App *a) { return camera_list_current_camera(a->cams); }
guint app_current_index(const App *a) { return camera_list_current(a->cams); }

guint app_retry_count(const App *a)
{
	guint r = 0;
	for (guint i = 0; i < a->ntiles; i++)
		r = MAX(r, a->t[i].retries);
	return r;
}

const char *app_last_error(const App *a)
{
	for (guint i = 0; i < a->ntiles; i++)
		if (a->t[i].error)
			return a->t[i].error;
	return NULL;
}

static guint grid_count(const App *a) { return a->cfg->grid_count; }

static const char *tile_name(const Tile *t)
{
	const Camera *c = camera_list_get(t->app->cams, t->cam);
	return c ? c->name : "?";
}

/* ---- state ---- */

static AppState aggregate(const App *a)
{
	gboolean live = FALSE, conn = FALSE, rec = FALSE;
	if (!a->visible)
		return APP_HIDDEN;
	for (guint i = 0; i < a->ntiles; i++) {
		live |= a->t[i].state == APP_LIVE;
		conn |= a->t[i].state == APP_CONNECTING;
		rec |= a->t[i].state == APP_RECONNECTING;
	}
	return live ? APP_LIVE : conn ? APP_CONNECTING : rec ? APP_RECONNECTING : APP_ERROR;
}

static void refresh_state(App *a)
{
	AppState s = aggregate(a);
	if (s == a->state)
		return;
	if (a->grid && a->visible)
		log_info("state %s -> %s (grid view, %u cameras)", app_state_name(a->state), app_state_name(s), a->ntiles);
	else
		log_info("state %s -> %s (camera '%s')", app_state_name(a->state), app_state_name(s),
			 app_current_camera(a)->name);
	a->state = s;
}

static void tile_set_error(Tile *t, const char *msg)
{
	g_free(t->error);
	t->error = msg ? log_redact(msg) : NULL;
}

static void tile_set_state(Tile *t, AppState s)
{
	App *a = t->app;
	if (t->state != s && a->grid)
		log_debug("tile %u ('%s'): %s -> %s", t->slot, tile_name(t), app_state_name(t->state), app_state_name(s));
	t->state = s;
	if (a->opt.overlay_enabled && a->visible && s != APP_HIDDEN)
		overlay_set_state(t->slot, s, t->error);
	refresh_state(a);
}

static char *build_url(void *user, guint slot, GError **err)
{
	App *a = user;
	const Camera *c = camera_list_get(a->cams, a->t[slot].cam);
	return camera_resolve_url(a->cfg, c, a->grid ? a->cfg->grid_subtype : -1, err);
}

static void on_first_frame(void *user, guint slot, guint session);
static void on_error(void *user, guint slot, guint session, const char *msg);
static void on_ended(void *user, guint slot, guint session);
static const VideoCallbacks VCB = { on_first_frame, on_error, on_ended };

static void stop_tile(Tile *t)
{
	if (t->retry_src) {
		g_source_remove(t->retry_src);
		t->retry_src = 0;
	}
	if (t->live_src) {
		g_source_remove(t->live_src);
		t->live_src = 0;
	}
	video_stop(t->slot);
	t->session = 0;
}

static void reset_tile(Tile *t)
{
	stop_tile(t);
	t->retries = 0;
	tile_set_error(t, NULL);
	t->state = APP_HIDDEN;
}

/* Begin a connection attempt for a tile (state must already allow it). */
static void start_tile(Tile *t)
{
	App *a = t->app;
	GError *e = NULL;

	stop_tile(t);
	t->session = video_start(t->slot, a->cfg, a->audio && !a->grid, build_url, a, &VCB, a, &e);
	if (!t->session) {
		/* Configuration-class failure (e.g. missing password): retrying cannot help. */
		tile_set_error(t, e ? e->message : "could not start the video stream");
		g_clear_error(&e);
		log_warn("cannot start stream for '%s': %s", tile_name(t), t->error);
		tile_set_state(t, APP_ERROR);
		return;
	}
	tile_set_state(t, APP_CONNECTING);
}

static gboolean retry_cb(gpointer data)
{
	Tile *t = data;
	t->retry_src = 0;
	if (t->state == APP_RECONNECTING)
		start_tile(t);
	return G_SOURCE_REMOVE;
}

static void stream_failed(Tile *t, const char *msg)
{
	App *a = t->app;
	stop_tile(t); /* no decode and no network while backing off */
	tile_set_error(t, msg);
	log_warn("stream problem on '%s': %s", tile_name(t), t->error);

	if (a->cfg->reconnect && t->retries < APP_MAX_RETRIES) {
		guint ms = (guint)(BACKOFF_MS[t->retries] * a->opt.backoff_scale);
		t->retries++;
		tile_set_state(t, APP_RECONNECTING);
		log_info("retry %u/%d in %u ms", t->retries, APP_MAX_RETRIES, ms);
		t->retry_src = g_timeout_add(ms ? ms : 1, retry_cb, t);
	} else {
		tile_set_state(t, APP_ERROR);
	}
}

static gboolean live_stable_cb(gpointer data)
{
	Tile *t = data;
	t->live_src = 0;
	if (t->state == APP_LIVE) {
		t->retries = 0;
		log_debug("stream stable; reconnect backoff reset");
	}
	return G_SOURCE_REMOVE;
}

static Tile *tile_for(App *a, guint slot, guint session)
{
	if (slot >= a->ntiles || !a->visible || session != a->t[slot].session)
		return NULL;
	return &a->t[slot];
}

static void on_first_frame(void *user, guint slot, guint session)
{
	App *a = user;
	Tile *t = tile_for(a, slot, session);
	if (!t || t->state != APP_CONNECTING)
		return;
	tile_set_error(t, NULL);
	tile_set_state(t, APP_LIVE);
	/* Do not reset the backoff on the first frame: a stream that delivers one frame and drops would
	 * otherwise restart at 0.5 s forever. Reset only after it has stayed live for stable_ms. */
	if (a->stable_ms == 0)
		t->retries = 0;
	else if (t->retries > 0 && !t->live_src)
		t->live_src = g_timeout_add(a->stable_ms, live_stable_cb, t);
}

static void on_error(void *user, guint slot, guint session, const char *msg)
{
	Tile *t = tile_for(user, slot, session);
	if (!t || (t->state != APP_CONNECTING && t->state != APP_LIVE))
		return;
	stream_failed(t, msg ? msg : "stream error");
}

static void on_ended(void *user, guint slot, guint session)
{
	on_error(user, slot, session, "stream ended");
}

/* Open or (re)connect the selected view as a user action: resets every tile's backoff. */
static void open_view(App *a)
{
	const char *labels[VIDEO_MAX_SLOTS + 1] = { NULL };

	for (guint i = 0; i < VIDEO_MAX_SLOTS; i++)
		reset_tile(&a->t[i]); /* the overlay may rebuild its tiles: no slot may stay alive */
	if (a->grid && grid_count(a) == 0)
		a->grid = FALSE; /* e.g. reload removed cameras */
	if (a->grid) {
		a->ntiles = grid_count(a);
		for (guint i = 0; i < a->ntiles; i++)
			a->t[i].cam = a->cfg->grid_idx[i];
	} else {
		a->ntiles = 1;
		a->t[0].cam = camera_list_current(a->cams);
	}
	for (guint i = 0; i < a->ntiles; i++)
		labels[i] = tile_name(&a->t[i]);

	a->visible = TRUE;
	a->start_ms = util_monotonic_ms();
	/* Idempotent: re-shows a window that exists, and re-creates one that was destroyed behind our
	 * back (monitor unplug, compositor closed the layer surface). Without this, the app could get
	 * stuck failing to start a stream because there is no GL area. */
	if (a->opt.overlay_enabled) {
		overlay_show(a->cfg, a->ntiles, labels);
		overlay_set_audio(a->audio, a->grid);
	}
	for (guint i = 0; i < a->ntiles; i++)
		start_tile(&a->t[i]); /* CONNECTING, or ERROR when the stream cannot even start */
	refresh_state(a);
}

/* ---- public ---- */

App *app_new(Config *cfg, const AppOptions *opt)
{
	App *a = g_new0(App, 1);
	a->cfg = cfg;
	a->cams = camera_list_new(cfg);
	a->opt = *opt;
	for (guint i = 0; i < VIDEO_MAX_SLOTS; i++) {
		a->t[i].app = a;
		a->t[i].slot = i;
	}
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
	a->audio = cfg->audio;
	a->grid = cfg->grid_count > 0;
	a->start_ms = util_monotonic_ms();
	log_set_level(cfg->log_level);
	return a;
}

void app_hide(App *a)
{
	if (!a->visible)
		return;
	for (guint i = 0; i < VIDEO_MAX_SLOTS; i++)
		reset_tile(&a->t[i]);
	a->visible = FALSE;
	refresh_state(a);
	a->ntiles = 0;
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
	g_free(a);
}

static void select_and_open(App *a, gboolean changed)
{
	switch (a->state) {
	case APP_HIDDEN:
	case APP_ERROR:
	case APP_RECONNECTING:
		open_view(a);
		break;
	case APP_CONNECTING:
	case APP_LIVE:
		if (changed)
			open_view(a);
		break;
	}
}

/* The view a closed overlay opens on: the grid when there is one. */
static void open_start_view(App *a)
{
	a->grid = grid_count(a) > 0;
	open_view(a);
}

gboolean app_toggle(App *a, GError **err)
{
	(void)err;
	if (!a->visible)
		open_start_view(a);
	else if (a->state == APP_ERROR)
		open_view(a); /* retry the same view */
	else
		app_hide(a);
	return TRUE;
}

gboolean app_show(App *a, const char *spec, GError **err)
{
	gboolean changed = a->grid;
	guint idx;

	if (spec && *spec) {
		if (!camera_list_find(a->cams, spec, &idx)) {
			app_fail(err, "unknown camera '%s' (see 'cctv-overlay list')", spec);
			return FALSE;
		}
		changed |= idx != camera_list_current(a->cams);
		camera_list_set_current(a->cams, idx);
	}
	a->grid = FALSE;
	select_and_open(a, changed);
	return TRUE;
}

gboolean app_grid(App *a, GError **err)
{
	if (grid_count(a) == 0) {
		app_fail(err, "no grid view: it needs at least 2 cameras and [grid] enabled = true");
		return FALSE;
	}
	gboolean changed = !a->grid;
	a->grid = TRUE;
	select_and_open(a, changed);
	return TRUE;
}

static void step(App *a, gboolean forward)
{
	guint n = camera_list_count(a->cams);

	if (grid_count(a) == 0) { /* plain camera ring */
		guint before = camera_list_current(a->cams);
		if (forward)
			camera_list_next(a->cams);
		else
			camera_list_prev(a->cams);
		select_and_open(a, before != camera_list_current(a->cams));
		return;
	}
	if (!a->visible) {
		open_start_view(a);
		return;
	}
	/* Ring: grid, camera 0, ..., camera n-1, back to the grid. */
	if (a->grid) {
		a->grid = FALSE;
		camera_list_set_current(a->cams, forward ? 0 : n - 1);
	} else if (camera_list_current(a->cams) == (forward ? n - 1 : 0)) {
		a->grid = TRUE;
	} else if (forward) {
		camera_list_next(a->cams);
	} else {
		camera_list_prev(a->cams);
	}
	select_and_open(a, TRUE);
}

gboolean app_next(App *a, GError **err)
{
	(void)err;
	step(a, TRUE);
	return TRUE;
}

gboolean app_prev(App *a, GError **err)
{
	(void)err;
	step(a, FALSE);
	return TRUE;
}

void app_set_audio(App *a, gboolean on)
{
	a->audio = on;
	log_info("audio %s", on ? "enabled" : "disabled");
	if (!a->visible)
		return;
	if (!a->grid && a->t[0].session)
		video_set_audio(0, on);
	if (a->opt.overlay_enabled)
		overlay_set_audio(on, a->grid);
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

	if (!a->visible) {
		a->grid = grid_count(a) > 0;
		return TRUE;
	}
	if (a->grid || a->state == APP_ERROR || a->state == APP_RECONNECTING || !found) {
		/* grid: tiles may have changed; error: e.g. the password was just fixed; gone: first camera */
		open_view(a);
	} else {
		a->t[0].cam = idx;
		const char *label = app_current_camera(a)->name;
		if (a->opt.overlay_enabled)
			overlay_show(a->cfg, 1, &label); /* same layout: relabels only */
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
	const char *err = app_last_error(a);

	json_object_set_boolean_member(o, "ok", TRUE);
	json_object_set_boolean_member(o, "visible", a->visible);
	json_object_set_string_member(o, "state", app_state_name(a->state));
	json_object_set_string_member(o, "view", a->grid ? "grid" : "camera");
	json_object_set_string_member(o, "camera", c->name);
	json_object_set_int_member(o, "camera_index", camera_list_current(a->cams));
	json_object_set_int_member(o, "camera_count", camera_list_count(a->cams));
	json_object_set_boolean_member(o, "audio", a->audio);
	if (err)
		json_object_set_string_member(o, "error", err);
	else
		json_object_set_null_member(o, "error");
	json_object_set_int_member(o, "retries", app_retry_count(a));
	json_object_set_int_member(o, "uptime_ms", util_monotonic_ms() - a->start_ms);

	JsonArray *tiles = json_array_new();
	for (guint i = 0; a->visible && i < a->ntiles; i++) {
		JsonObject *to = json_object_new();
		json_object_set_string_member(to, "camera", tile_name(&a->t[i]));
		json_object_set_string_member(to, "state", app_state_name(a->t[i].state));
		json_array_add_object_element(tiles, to);
	}
	json_object_set_array_member(o, "tiles", tiles);
	return o;
}

static JsonObject *list_object(App *a)
{
	JsonObject *o = json_object_new();
	JsonArray *arr = json_array_new();

	for (guint i = 0; i < camera_list_count(a->cams); i++) {
		const Camera *c = camera_list_get(a->cams, i);
		JsonObject *co = json_object_new();
		gboolean in_grid = FALSE;
		for (guint g = 0; g < a->cfg->grid_count; g++)
			in_grid |= a->cfg->grid_idx[g] == i;
		json_object_set_int_member(co, "index", i);
		json_object_set_string_member(co, "id", c->id);
		json_object_set_string_member(co, "name", c->name);
		json_object_set_int_member(co, "channel", c->channel);
		json_object_set_boolean_member(co, "current", i == camera_list_current(a->cams));
		json_object_set_boolean_member(co, "in_grid", in_grid);
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
	} else if (strcmp(cmd, "grid") == 0) {
		ok = app_grid(a, &e);
	} else if (strcmp(cmd, "hide") == 0) {
		app_hide(a);
	} else if (strcmp(cmd, "show") == 0) {
		const char *spec = NULL;
		JsonNode *n = json_object_get_member(req, "camera");
		if (n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING)
			spec = json_node_get_string(n);
		ok = app_show(a, spec, &e);
	} else if (strcmp(cmd, "audio") == 0) {
		/* optional "mode": "on" | "off" | "toggle" (default) */
		const char *mode = NULL;
		JsonNode *n = json_object_get_member(req, "mode");
		if (n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING)
			mode = json_node_get_string(n);
		if (!mode || strcmp(mode, "toggle") == 0)
			app_set_audio(a, !a->audio);
		else if (strcmp(mode, "on") == 0)
			app_set_audio(a, TRUE);
		else if (strcmp(mode, "off") == 0)
			app_set_audio(a, FALSE);
		else {
			ok = FALSE;
			app_fail(&e, "audio: use on, off or toggle");
		}
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
