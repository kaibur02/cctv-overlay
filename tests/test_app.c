#include "app.h"
#include "logging.h"
#include "testlib.h"

#include <string.h>

/* Stub inspection hooks (video_stub.c / overlay_stub.c). */
gboolean video_stub_active(void);
guint video_stub_start_count(void);
gboolean overlay_stub_visible(void);
const char *overlay_stub_label(void);
void overlay_stub_simulate_lost(void);

#define PW "p@ss:w/rd%1"

static void sink(LogLevel l, const char *line, void *u)
{
	(void)l;
	(void)u;
	g_assert_null(strstr(line, "p@ss"));
	g_assert_null(strstr(line, "rd%1"));
}

static char *g_dir, *g_path;
static gboolean g_quit_called;

static void quit_cb(void *u)
{
	(void)u;
	g_quit_called = TRUE;
}

static App *make_app(const char *text, const char *password, gboolean overlay)
{
	g_unsetenv("CCTV_OVERLAY_PASSWORD");
	g_unsetenv("CCTV_OVERLAY_USERNAME");
	g_unsetenv("CCTV_OVERLAY_STUB_FAIL");
	g_unsetenv("CCTV_OVERLAY_STUB_DROP_AFTER_MS");
	g_setenv("CCTV_OVERLAY_STUB_DELAY_MS", "10", TRUE);
	if (!g_getenv("CCTV_OVERLAY_LIVE_STABLE_MS"))
		g_setenv("CCTV_OVERLAY_LIVE_STABLE_MS", "0", TRUE); /* unit tests: reset backoff at once */
	if (password)
		g_setenv("CCTV_OVERLAY_PASSWORD", password, TRUE);
	log_set_sink(sink, NULL);
	log_set_level(LOG_LVL_DEBUG);
	g_path = tl_write_config(text, NULL, 0, &g_dir);
	GError *e = NULL;
	Config *c = config_load(g_path, &e);
	g_assert_no_error(e);
	AppOptions o = { .overlay_enabled = overlay, .backoff_scale = 0.01, .quit_cb = quit_cb };
	return app_new(c, &o);
}

static void destroy(App *a)
{
	app_free(a);
	g_assert_false(video_stub_active());
	g_assert_false(overlay_stub_visible());
	log_set_sink(NULL, NULL);
	log_clear_secrets();
	g_unsetenv("CCTV_OVERLAY_PASSWORD");
	g_unsetenv("CCTV_OVERLAY_STUB_FAIL");
	g_unsetenv("CCTV_OVERLAY_LIVE_STABLE_MS");
	tl_rm_dir(g_dir);
	g_free(g_path);
}

static gboolean wait_state(App *a, AppState want, guint timeout_ms)
{
	gint64 end = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
	while (app_state(a) != want && g_get_monotonic_time() < end)
		g_main_context_iteration(NULL, FALSE), g_usleep(500);
	return app_state(a) == want;
}

static void pump(guint ms)
{
	gint64 end = g_get_monotonic_time() + (gint64)ms * 1000;
	while (g_get_monotonic_time() < end)
		g_main_context_iteration(NULL, FALSE), g_usleep(500);
}

static void test_toggle_cycle(void)
{
	App *a = make_app(TL_GOOD_CONFIG, PW, TRUE);
	g_assert_cmpint(app_state(a), ==, APP_HIDDEN);
	g_assert_false(video_stub_active());

	g_assert_true(app_toggle(a, NULL));
	g_assert_cmpint(app_state(a), ==, APP_CONNECTING);
	g_assert_true(video_stub_active());
	g_assert_true(overlay_stub_visible());
	g_assert_cmpstr(overlay_stub_label(), ==, "Front Door");
	g_assert_true(wait_state(a, APP_LIVE, 1000));
	g_assert_null(app_last_error(a));

	g_assert_true(app_toggle(a, NULL)); /* LIVE -> HIDDEN */
	g_assert_cmpint(app_state(a), ==, APP_HIDDEN);
	g_assert_false(video_stub_active()); /* hidden = no stream */
	g_assert_false(overlay_stub_visible());
	destroy(a);
}

static void test_next_prev_while_hidden_opens(void)
{
	App *a = make_app(TL_GOOD_CONFIG, PW, TRUE);
	g_assert_true(app_next(a, NULL));
	g_assert_cmpint(app_state(a), ==, APP_CONNECTING); /* opened on the neighbour */
	g_assert_cmpuint(app_current_index(a), ==, 1);
	g_assert_cmpstr(overlay_stub_label(), ==, "Driveway");
	g_assert_true(wait_state(a, APP_LIVE, 1000));

	app_next(a, NULL); /* switch while visible: reconnect */
	g_assert_cmpuint(app_current_index(a), ==, 2);
	g_assert_cmpint(app_state(a), ==, APP_CONNECTING);
	g_assert_cmpstr(overlay_stub_label(), ==, "Garden");
	app_next(a, NULL); /* wrap */
	g_assert_cmpuint(app_current_index(a), ==, 0);
	app_prev(a, NULL); /* wrap back */
	g_assert_cmpuint(app_current_index(a), ==, 2);
	g_assert_true(wait_state(a, APP_LIVE, 1000));

	app_hide(a);
	g_assert_cmpint(app_state(a), ==, APP_HIDDEN);
	app_prev(a, NULL); /* hidden + prev opens on previous */
	g_assert_cmpuint(app_current_index(a), ==, 1);
	g_assert_cmpint(app_state(a), ==, APP_CONNECTING);
	destroy(a);
}

static void test_show(void)
{
	App *a = make_app(TL_GOOD_CONFIG, PW, TRUE);
	GError *e = NULL;
	g_assert_false(app_show(a, "nope", &e));
	g_assert_nonnull(e);
	g_clear_error(&e);
	g_assert_cmpint(app_state(a), ==, APP_HIDDEN);

	g_assert_true(app_show(a, "garden", NULL));
	g_assert_cmpuint(app_current_index(a), ==, 2);
	g_assert_true(wait_state(a, APP_LIVE, 1000));
	guint starts = video_stub_start_count();
	g_assert_true(app_show(a, "Garden", NULL)); /* same camera while live: no-op */
	g_assert_cmpuint(video_stub_start_count(), ==, starts);
	g_assert_cmpint(app_state(a), ==, APP_LIVE);
	g_assert_true(app_show(a, NULL, NULL));
	g_assert_cmpint(app_state(a), ==, APP_LIVE);
	destroy(a);
}

static void test_hide_during_connecting_cancels(void)
{
	g_setenv("CCTV_OVERLAY_STUB_DELAY_MS", "50", TRUE);
	App *a = make_app(TL_GOOD_CONFIG, PW, TRUE);
	g_setenv("CCTV_OVERLAY_STUB_DELAY_MS", "50", TRUE);
	app_toggle(a, NULL);
	g_assert_cmpint(app_state(a), ==, APP_CONNECTING);
	app_hide(a);
	g_assert_cmpint(app_state(a), ==, APP_HIDDEN);
	g_assert_false(video_stub_active());
	pump(120);
	g_assert_cmpint(app_state(a), ==, APP_HIDDEN); /* the late first frame never arrives */
	destroy(a);
}

static void test_failure_retries_then_error(void)
{
	App *a = make_app(TL_GOOD_CONFIG, PW, TRUE);
	g_setenv("CCTV_OVERLAY_STUB_FAIL", "1", TRUE);
	app_toggle(a, NULL);
	g_assert_true(wait_state(a, APP_RECONNECTING, 1000));
	g_assert_cmpuint(app_retry_count(a), ==, 1);
	g_assert_false(video_stub_active()); /* no stream while backing off */
	g_assert_nonnull(app_last_error(a));
	g_assert_true(wait_state(a, APP_ERROR, 3000));
	g_assert_cmpuint(app_retry_count(a), ==, APP_MAX_RETRIES);
	g_assert_false(video_stub_active());
	g_assert_nonnull(strstr(app_last_error(a), "simulated"));
	g_assert_cmpuint(video_stub_start_count(), >=, 1 + APP_MAX_RETRIES);
	g_assert_true(overlay_stub_visible()); /* ERROR keeps the overlay up */

	/* ERROR --toggle--> CONNECTING, with the backoff reset */
	g_unsetenv("CCTV_OVERLAY_STUB_FAIL");
	app_toggle(a, NULL);
	g_assert_cmpint(app_state(a), ==, APP_CONNECTING);
	g_assert_cmpuint(app_retry_count(a), ==, 0);
	g_assert_true(wait_state(a, APP_LIVE, 1000));
	g_assert_null(app_last_error(a));
	destroy(a);
}

static void test_recovery_resets_backoff(void)
{
	App *a = make_app(TL_GOOD_CONFIG, PW, FALSE);
	g_setenv("CCTV_OVERLAY_STUB_FAIL", "first:2", TRUE);
	app_toggle(a, NULL);
	g_assert_true(wait_state(a, APP_RECONNECTING, 1000));
	g_assert_true(wait_state(a, APP_LIVE, 2000)); /* third attempt succeeds */
	g_assert_cmpuint(app_retry_count(a), ==, 0);
	g_assert_false(overlay_stub_visible()); /* --no-overlay: overlay never touched */
	destroy(a);
}

static void test_reconnect_disabled(void)
{
	App *a = make_app("[stream]\nreconnect = false\n" TL_GOOD_CONFIG, PW, TRUE);
	g_setenv("CCTV_OVERLAY_STUB_FAIL", "1", TRUE);
	app_toggle(a, NULL);
	g_assert_true(wait_state(a, APP_ERROR, 1000));
	g_assert_cmpuint(app_retry_count(a), ==, 0);
	destroy(a);
}

static void test_live_drop_reconnects(void)
{
	App *a = make_app(TL_GOOD_CONFIG, PW, TRUE);
	g_setenv("CCTV_OVERLAY_STUB_DROP_AFTER_MS", "30", TRUE);
	app_toggle(a, NULL);
	g_assert_true(wait_state(a, APP_LIVE, 1000));
	g_assert_true(wait_state(a, APP_RECONNECTING, 1000)); /* LIVE -> RECONNECTING */
	g_unsetenv("CCTV_OVERLAY_STUB_DROP_AFTER_MS");
	g_assert_true(wait_state(a, APP_LIVE, 2000));
	destroy(a);
}

static void test_overlay_lost_while_live_recovers(void)
{
	App *a = make_app(TL_GOOD_CONFIG, PW, TRUE);
	g_assert_true(app_toggle(a, NULL));
	g_assert_true(wait_state(a, APP_LIVE, 1000));

	overlay_stub_simulate_lost(); /* compositor destroyed the layer surface */
	g_assert_false(overlay_stub_visible());

	g_assert_true(app_next(a, NULL));
	g_assert_cmpint(app_state(a), ==, APP_CONNECTING);
	g_assert_true(overlay_stub_visible()); /* re-created instead of staying stuck */
	g_assert_cmpstr(overlay_stub_label(), ==, "Driveway");
	g_assert_true(wait_state(a, APP_LIVE, 1000));
	destroy(a);
}

static void test_flapping_stream_reaches_error(void)
{
	g_setenv("CCTV_OVERLAY_LIVE_STABLE_MS", "1000", TRUE);
	App *a = make_app(TL_GOOD_CONFIG, PW, TRUE);
	g_setenv("CCTV_OVERLAY_STUB_DELAY_MS", "10", TRUE);
	g_setenv("CCTV_OVERLAY_STUB_DROP_AFTER_MS", "10", TRUE);
	guint starts_before = video_stub_start_count();
	app_toggle(a, NULL);
	g_assert_true(wait_state(a, APP_ERROR, 8000)); /* one frame per connection must not loop forever */
	g_assert_cmpuint(app_retry_count(a), ==, APP_MAX_RETRIES);
	g_assert_cmpuint(video_stub_start_count() - starts_before, <=, 1 + APP_MAX_RETRIES);
	destroy(a);
	g_unsetenv("CCTV_OVERLAY_STUB_DROP_AFTER_MS");
}

static void test_reload_in_error_retries(void)
{
	App *a = make_app(TL_GOOD_CONFIG, PW, TRUE);
	g_setenv("CCTV_OVERLAY_STUB_FAIL", "1", TRUE);
	app_toggle(a, NULL);
	g_assert_true(wait_state(a, APP_ERROR, 5000));
	g_unsetenv("CCTV_OVERLAY_STUB_FAIL");
	g_assert_true(app_reload(a, NULL)); /* e.g. the password was fixed */
	g_assert_cmpint(app_state(a), ==, APP_CONNECTING);
	g_assert_true(wait_state(a, APP_LIVE, 2000));
	destroy(a);
}

static void test_missing_password_only_when_streaming(void)
{
	App *a = make_app(TL_GOOD_CONFIG, NULL, TRUE); /* no password anywhere */
	g_assert_cmpint(app_state(a), ==, APP_HIDDEN); /* app is fine without it */
	JsonObject *st = app_ipc_handler(a, "status", NULL);
	g_assert_true(json_object_get_boolean_member(st, "ok"));
	json_object_unref(st);

	app_toggle(a, NULL);
	g_assert_cmpint(app_state(a), ==, APP_ERROR); /* immediate, no pointless retries */
	g_assert_nonnull(strstr(app_last_error(a), "password"));
	g_assert_cmpuint(app_retry_count(a), ==, 0);
	g_assert_false(video_stub_active());
	app_hide(a);
	g_assert_cmpint(app_state(a), ==, APP_HIDDEN);
	destroy(a);
}

static void test_reload(void)
{
	App *a = make_app(TL_GOOD_CONFIG, PW, TRUE);
	app_show(a, "driveway", NULL);
	g_assert_true(wait_state(a, APP_LIVE, 1000));

	/* new file: cameras reordered, driveway kept */
	g_assert_true(g_file_set_contents(g_path,
		"[recorder]\nhost = h.example.invalid\nusername = tester\n"
		"[camera.garden]\nname = Garden\nchannel = 3\n"
		"[camera.driveway]\nname = Driveway\nchannel = 2\n", -1, NULL));
	guint starts = video_stub_start_count();
	g_assert_true(app_reload(a, NULL));
	g_assert_cmpuint(app_current_index(a), ==, 1); /* still Driveway, new position */
	g_assert_cmpstr(app_current_camera(a)->name, ==, "Driveway");
	g_assert_cmpint(app_state(a), ==, APP_LIVE);
	g_assert_cmpuint(video_stub_start_count(), ==, starts); /* no needless reconnect */

	/* current camera removed: select first and reconnect */
	g_assert_true(g_file_set_contents(g_path,
		"[recorder]\nhost = h.example.invalid\nusername = tester\n"
		"[camera.garden]\nname = Garden\nchannel = 3\n", -1, NULL));
	g_assert_true(app_reload(a, NULL));
	g_assert_cmpuint(app_current_index(a), ==, 0);
	g_assert_cmpstr(overlay_stub_label(), ==, "Garden");
	g_assert_cmpint(app_state(a), ==, APP_CONNECTING);

	/* broken file: error, previous config kept */
	g_assert_true(g_file_set_contents(g_path, "[overlay]\nwidth = 1\n", -1, NULL));
	GError *e = NULL;
	g_assert_false(app_reload(a, &e));
	g_assert_nonnull(e);
	g_error_free(e);
	g_assert_cmpstr(app_current_camera(a)->name, ==, "Garden");
	destroy(a);
}

static void test_reload_hidden_keeps_hidden(void)
{
	App *a = make_app(TL_GOOD_CONFIG, PW, TRUE);
	g_assert_true(app_reload(a, NULL));
	g_assert_cmpint(app_state(a), ==, APP_HIDDEN);
	g_assert_false(video_stub_active());
	destroy(a);
}

static void test_ipc_handler_json(void)
{
	App *a = make_app(TL_GOOD_CONFIG, PW, TRUE);
	JsonObject *r = app_ipc_handler(a, "next", NULL);
	g_assert_true(json_object_get_boolean_member(r, "ok"));
	g_assert_true(json_object_get_boolean_member(r, "visible"));
	g_assert_cmpstr(json_object_get_string_member(r, "state"), ==, "connecting");
	g_assert_cmpstr(json_object_get_string_member(r, "camera"), ==, "Driveway");
	g_assert_cmpint(json_object_get_int_member(r, "camera_index"), ==, 1);
	g_assert_cmpint(json_object_get_int_member(r, "camera_count"), ==, 3);
	g_assert_true(json_object_get_null_member(r, "error"));
	json_object_unref(r);

	r = app_ipc_handler(a, "list", NULL);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(r, "cameras")), ==, 3);
	json_object_unref(r);

	r = app_ipc_handler(a, "bogus", NULL);
	g_assert_false(json_object_get_boolean_member(r, "ok"));
	json_object_unref(r);

	g_quit_called = FALSE;
	r = app_ipc_handler(a, "quit", NULL);
	json_object_unref(r);
	g_assert_false(g_quit_called); /* only after the reply is written */
	app_ipc_after_reply(a, "quit");
	g_assert_true(g_quit_called);
	destroy(a);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/app/toggle-cycle", test_toggle_cycle);
	g_test_add_func("/app/next-prev-while-hidden-opens", test_next_prev_while_hidden_opens);
	g_test_add_func("/app/show", test_show);
	g_test_add_func("/app/hide-during-connecting", test_hide_during_connecting_cancels);
	g_test_add_func("/app/failure-retries-then-error", test_failure_retries_then_error);
	g_test_add_func("/app/recovery-resets-backoff", test_recovery_resets_backoff);
	g_test_add_func("/app/reconnect-disabled", test_reconnect_disabled);
	g_test_add_func("/app/live-drop-reconnects", test_live_drop_reconnects);
	g_test_add_func("/app/overlay-lost-while-live", test_overlay_lost_while_live_recovers);
	g_test_add_func("/app/flapping-stream-bounded", test_flapping_stream_reaches_error);
	g_test_add_func("/app/reload-in-error", test_reload_in_error_retries);
	g_test_add_func("/app/missing-password", test_missing_password_only_when_streaming);
	g_test_add_func("/app/reload", test_reload);
	g_test_add_func("/app/reload-hidden", test_reload_hidden_keeps_hidden);
	g_test_add_func("/app/ipc-handler-json", test_ipc_handler_json);
	return g_test_run();
}
