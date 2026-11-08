#include "config.h"
#include "testlib.h"

#include <stdlib.h>
#include <string.h>

static GString *warnings;

static void sink(LogLevel l, const char *line, void *u)
{
	(void)l;
	(void)u;
	g_string_append(warnings, line);
	g_string_append_c(warnings, '\n');
}

static void fixture_begin(void)
{
	warnings = g_string_new(NULL);
	log_set_sink(sink, NULL);
	g_unsetenv("CCTV_OVERLAY_PASSWORD");
	g_unsetenv("CCTV_OVERLAY_USERNAME");
}

static void fixture_end(void)
{
	log_set_sink(NULL, NULL);
	g_string_free(warnings, TRUE);
	log_clear_secrets();
}

/* Load `text`; if want_err is non-NULL the load must fail and the message must contain it. */
static Config *load(const char *text, const char *want_err)
{
	char *dir;
	char *path = tl_write_config(text, NULL, 0, &dir);
	GError *e = NULL;
	Config *c = config_load(path, &e);
	if (want_err) {
		g_assert_null(c);
		g_assert_nonnull(e);
		if (!strstr(e->message, want_err))
			g_error("expected error containing '%s' but got '%s'", want_err, e->message);
		g_assert_nonnull(strstr(e->message, path)); /* message names the file */
		g_error_free(e);
	} else {
		g_assert_no_error(e);
		g_assert_nonnull(c);
	}
	g_free(path);
	tl_rm_dir(dir);
	return c;
}

static void test_defaults_and_order(void)
{
	fixture_begin();
	Config *c = load(TL_GOOD_CONFIG, NULL);
	g_assert_cmpint(c->width, ==, 480);
	g_assert_cmpint(c->height, ==, 270);
	g_assert_cmpstr(c->anchor, ==, "top-right");
	g_assert_cmpstr(c->layer, ==, "overlay");
	g_assert_cmpint(c->port, ==, 554);
	g_assert_cmpint(c->subtype, ==, 1);
	g_assert_cmpstr(c->transport, ==, "tcp");
	g_assert_cmpstr(c->username, ==, "tester");
	g_assert_null(c->password);
	g_assert_cmpuint(c->cameras->len, ==, 3);
	const char *want[] = { "front", "driveway", "garden" };
	for (guint i = 0; i < 3; i++)
		g_assert_cmpstr(((Camera *)g_ptr_array_index(c->cameras, i))->id, ==, want[i]);
	g_assert_cmpint(((Camera *)g_ptr_array_index(c->cameras, 0))->subtype, ==, -1);
	g_assert_cmpint(((Camera *)g_ptr_array_index(c->cameras, 1))->subtype, ==, 0);
	config_free(c);
	fixture_end();
}

static void test_shipped_example(void)
{
	fixture_begin();
	GError *e = NULL;
	Config *c = config_load("examples/config.example.ini", &e);
	g_assert_no_error(e);
	g_assert_nonnull(c);
	g_assert_cmpuint(c->cameras->len, ==, 2);
	g_assert_null(c->password); /* the example dir has no secrets.ini; example secrets is named *.example.ini */
	config_free(c);
	fixture_end();
}

static void test_full_values(void)
{
	fixture_begin();
	Config *c = load("[general]\nlog_level = debug\nmonitor = DP-1\nstart_hidden = false\n"
			 "[overlay]\nwidth = 640\nheight = 360\nanchor = bottom-left\nmargin_x = 0\nmargin_y = 10\n"
			 "layer = top\nshow_label = false\nopacity = 0.5\n"
			 "[stream]\ntransport = udp\nsubtype = 0\nhwdec = no\nreconnect = false\n"
			 "[recorder]\nhost = nvr.example.invalid\nport = 8554\npath = /x\n"
			 "[camera.a]\nname = A\nchannel = 4\n",
			 NULL);
	g_assert_cmpint(c->log_level, ==, LOG_LVL_DEBUG);
	g_assert_cmpstr(c->monitor, ==, "DP-1");
	g_assert_false(c->start_hidden);
	g_assert_cmpint(c->width, ==, 640);
	g_assert_cmpstr(c->anchor, ==, "bottom-left");
	g_assert_cmpstr(c->layer, ==, "top");
	g_assert_false(c->show_label);
	g_assert_cmpfloat(c->opacity, ==, 0.5);
	g_assert_cmpstr(c->transport, ==, "udp");
	g_assert_false(c->reconnect);
	g_assert_cmpint(c->port, ==, 8554);
	config_free(c);
	fixture_end();
}

static void test_bad_inputs(void)
{
	fixture_begin();
	const char *R = "[recorder]\nhost = h.example.invalid\n";
	struct { const char *text; const char *err; } bad[] = {
		{ "", "no cameras defined" },
		{ "[recorder]\nhost = h\n", "no cameras defined" },
		{ "[recorder]\nhost = h\n[camera.a]\nchannel = 1\n", "[camera.a] name: required" },
		{ "[recorder]\nhost = h\n[camera.a]\nname = A\n", "channel: required" },
		{ "[recorder]\nhost = h\n[camera.a]\nname = A\nchannel = 0\n", "out of range (1..1024)" },
		{ "[recorder]\nhost = h\n[camera.a]\nname = A\nchannel = x\n", "must be an integer" },
		{ "[recorder]\nhost = h\n[camera.a]\nname = A\nchannel = 1\n[camera.b]\nname = a\nchannel = 2\n", "duplicates [camera.a]" },
		{ "[camera.a]\nname = A\nchannel = 1\n", "[recorder] host: required" },
		{ "[recorder]\nhost =\n[camera.a]\nname = A\nchannel = 1\n", "[recorder] host: must not be empty" },
		{ "[recorder]\nhost = a b\n[camera.a]\nname = A\nchannel = 1\n", "not a valid host" },
		{ "[recorder]\nhost = h\nport = 0\n[camera.a]\nname = A\nchannel = 1\n", "port: 0 is out of range" },
		{ "[recorder]\nhost = h\nport = 70000\n[camera.a]\nname = A\nchannel = 1\n", "70000 is out of range" },
		{ "[recorder]\nhost = h\npath = nope\n[camera.a]\nname = A\nchannel = 1\n", "must start with '/'" },
		{ "[recorder]\nhost = h\npassword = x\n[camera.a]\nname = A\nchannel = 1\n", "not allowed in config.ini" },
		{ "[recorder]\nhost = h\nhots = x\n[camera.a]\nname = A\nchannel = 1\n", "unknown key 'hots'" },
		{ "[bogus]\nx = 1\n", "unknown section [bogus]" },
		{ "[camera.]\nname = A\n", "needs an id" },
		{ "[recorder]\nhost = h\n[camera.a]\nname = A\nchannel = 1\nurl = http://x/\n", "must start with rtsp://" },
		{ "[general]\nlog_level = loud\n", "log_level" },
		{ "[general]\nstart_hidden = maybe\n", "must be true or false" },
		{ "[overlay]\nanchor = middle\n", "'middle' is not valid; use one of: top-left | top-right" },
		{ "[overlay]\nlayer = background\n", "layer: 'background' is not valid" },
		{ "[overlay]\nwidth = 10\n", "width: 10 is out of range" },
		{ "[overlay]\nheight = 99999\n", "height: 99999 is out of range" },
		{ "[overlay]\nmargin_x = -1\n", "margin_x: -1 is out of range" },
		{ "[overlay]\nopacity = 2\n", "opacity: 2 is out of range" },
		{ "[overlay]\nopacity = abc\n", "opacity: must be a number" },
		{ "[stream]\ntransport = quic\n", "transport: 'quic' is not valid" },
		{ "[stream]\nhwdec = magic\n", "hwdec: 'magic' is not valid" },
		{ "[stream]\nsubtype = 9\n", "subtype: 9 is out of range" },
		{ "[overlay]\nwidth = 480 ; comment\n", "must be an integer" },
		{ "this is not ini\n", "which is not a key-value pair" },
	};
	for (guint i = 0; i < G_N_ELEMENTS(bad); i++) {
		g_test_message("case %u", i);
		/* sections lacking cameras need R to reach the field under test */
		(void)R;
		load(bad[i].text, bad[i].err);
	}
	fixture_end();
}

static void test_missing_file(void)
{
	fixture_begin();
	GError *e = NULL;
	Config *c = config_load("/nonexistent-dir-xyz/config.ini", &e);
	g_assert_null(c);
	g_assert_nonnull(strstr(e->message, "not found"));
	g_assert_nonnull(strstr(e->message, "config.example.ini"));
	g_error_free(e);
	fixture_end();
}

static void test_url_only_camera(void)
{
	fixture_begin();
	Config *c = load("[camera.x]\nname = X\nurl = rtsp://h.example.invalid/s\n", NULL);
	g_assert_null(c->host); /* no default recorder host in code */
	config_free(c);
	fixture_end();
}

static void test_secrets_file_and_env(void)
{
	fixture_begin();
	char *dir;
	char *path = tl_write_config(TL_GOOD_CONFIG, "[recorder]\npassword = p@ss:w/rd%1\\x\nusername = fileuser\n", 0600, &dir);
	GError *e = NULL;
	Config *c = config_load(path, &e);
	g_assert_no_error(e);
	g_assert_cmpstr(c->password, ==, "p@ss:w/rd%1\\x"); /* literal, no escape processing */
	g_assert_cmpstr(c->username, ==, "fileuser");
	g_assert_null(strstr(warnings->str, "mode")); /* 0600: no warning */
	config_free(c);

	g_setenv("CCTV_OVERLAY_PASSWORD", "envpass", TRUE);
	g_setenv("CCTV_OVERLAY_USERNAME", "envuser", TRUE);
	c = config_load(path, &e);
	g_assert_cmpstr(c->password, ==, "envpass"); /* env wins */
	g_assert_cmpstr(c->username, ==, "envuser");
	config_free(c);
	g_free(path);
	tl_rm_dir(dir);
	fixture_end();
}

static void test_secrets_permission_warning(void)
{
	fixture_begin();
	char *dir;
	char *path = tl_write_config(TL_GOOD_CONFIG, "[recorder]\npassword = hunter2\n", 0644, &dir);
	GError *e = NULL;
	Config *c = config_load(path, &e);
	g_assert_no_error(e); /* warn, do not fail */
	g_assert_nonnull(c);
	g_assert_nonnull(strstr(warnings->str, "secrets.ini"));
	g_assert_nonnull(strstr(warnings->str, "0644"));
	g_assert_null(strstr(warnings->str, "hunter2"));
	config_free(c);
	g_free(path);
	tl_rm_dir(dir);
	fixture_end();
}

static void test_malformed_secrets_does_not_leak(void)
{
	fixture_begin();
	char *dir;
	char *path = tl_write_config(TL_GOOD_CONFIG, "[recorder]\nhunter2-no-equals-sign\n", 0600, &dir);
	GError *e = NULL;
	Config *c = config_load(path, &e);
	g_assert_nonnull(c); /* secrets problems never prevent load; password is just missing */
	g_assert_null(c->password);
	g_assert_null(strstr(warnings->str, "hunter2"));
	g_assert_nonnull(strstr(warnings->str, "could not be parsed"));
	config_free(c);
	g_free(path);
	tl_rm_dir(dir);
	fixture_end();
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/config/defaults-and-order", test_defaults_and_order);
	g_test_add_func("/config/shipped-example", test_shipped_example);
	g_test_add_func("/config/full-values", test_full_values);
	g_test_add_func("/config/bad-inputs", test_bad_inputs);
	g_test_add_func("/config/missing-file", test_missing_file);
	g_test_add_func("/config/url-only-camera", test_url_only_camera);
	g_test_add_func("/config/secrets-file-and-env", test_secrets_file_and_env);
	g_test_add_func("/config/secrets-permission-warning", test_secrets_permission_warning);
	g_test_add_func("/config/malformed-secrets-no-leak", test_malformed_secrets_does_not_leak);
	return g_test_run();
}
