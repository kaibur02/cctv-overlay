#include "config.h"
#include "logging.h"

#include <glib.h>
#include <glib/gstdio.h>

#define PW "p@ss:w/rd%1"

static GString *captured;

static void sink(LogLevel l, const char *line, void *user)
{
	(void)l;
	(void)user;
	g_string_append(captured, line);
	g_string_append_c(captured, '\n');
}

static void setup(void)
{
	log_clear_secrets();
	log_set_level(LOG_LVL_DEBUG);
	captured = g_string_new(NULL);
	log_set_sink(sink, NULL);
}

static void teardown(void)
{
	log_set_sink(NULL, NULL);
	g_string_free(captured, TRUE);
	log_clear_secrets();
}

static void test_url_userinfo(void)
{
	setup();
	char *r = log_redact("open rtsp://tester:hunter2@192.168.1.100:554/cam?channel=1 now");
	g_assert_cmpstr(r, ==, "open rtsp://***:***@192.168.1.100:554/cam?channel=1 now");
	g_free(r);
	r = log_redact("see https://user@host/x and http://a:b@c");
	g_assert_cmpstr(r, ==, "see https://***:***@host/x and http://***:***@c");
	g_free(r);
	r = log_redact("no url here, mail me@example.invalid");
	g_assert_cmpstr(r, ==, "no url here, mail me@example.invalid");
	g_free(r);
	teardown();
}

static void test_registered_secret_forms(void)
{
	setup();
	log_register_secret(PW);
	char *r = log_redact("raw " PW " encoded p%40ss%3Aw%2Frd%251 end");
	g_assert_cmpstr(r, ==, "raw *** encoded *** end");
	g_free(r);
	/* URL carrying the encoded password: both mechanisms apply */
	r = log_redact("rtsp://tester:p%40ss%3Aw%2Frd%251@host:554/x");
	g_assert_null(strstr(r, "p%40ss"));
	g_assert_null(strstr(r, "tester"));
	g_free(r);
	/* URL with the RAW awkward password (contains '/', ':' and '@') */
	r = log_redact("rtsp://tester:" PW "@host:554/x");
	g_assert_null(strstr(r, "p@ss"));
	g_assert_null(strstr(r, "w/rd"));
	g_assert_null(strstr(r, "rd%1"));
	g_free(r);
	teardown();
}

static void test_log_msg_redacts(void)
{
	setup();
	log_register_secret(PW);
	log_info("connecting with %s to %s", PW, "rtsp://tester:abc@h/p");
	log_debug("debug %s", PW);
	g_assert_null(strstr(captured->str, "p@ss"));
	g_assert_null(strstr(captured->str, "abc"));
	g_assert_nonnull(strstr(captured->str, "***"));
	teardown();
}

static void test_redaction_hard_cases(void)
{
	setup();
	/* Invalid UTF-8 must not make the URL rule fail open. */
	char bad[] = "bad \xff\xfe rtsp://u3:pw3@h/p";
	char *r = log_redact(bad);
	g_assert_null(strstr(r, "pw3"));
	g_free(r);

	/* Authorization headers (Basic base64 or Digest) must be removed wholesale. */
	r = log_redact("Authorization: Basic dXNlcjpwYXNz\r\nCSeq: 2");
	g_assert_cmpstr(r, ==, "Authorization: ***\r\nCSeq: 2");
	g_free(r);
	r = log_redact("Proxy-Authorization: Digest username=\"u\", response=\"abc\"");
	g_assert_cmpstr(r, ==, "Authorization: ***");
	g_free(r);

	/* A raw '/' in userinfo must not stop the URL rule early. */
	r = log_redact("rtsp://u:zz/yy@h:554/p");
	g_assert_null(strstr(r, "zz/yy"));
	g_free(r);

	/* Control characters are neutralised in emitted lines. */
	log_info("esc \x1b[31mRED\x1b[0m end");
	g_assert_null(strchr(captured->str, '\x1b'));
	g_assert_nonnull(strstr(captured->str, "?[31m"));
	teardown();
}

static void test_url_override_secrets_registered(void)
{
	setup();
	g_unsetenv("CCTV_OVERLAY_PASSWORD");
	g_unsetenv("CCTV_OVERLAY_USERNAME");

	char *dir = g_dir_make_tmp("cctv-log-XXXXXX", NULL);
	g_assert_nonnull(dir);
	char *path = g_build_filename(dir, "config.ini", NULL);
	const char *cfg = "[camera.a]\nname = A\nurl = rtsp://tester:zz/yy@127.0.0.1:1/live\n";
	g_assert_true(g_file_set_contents(path, cfg, -1, NULL));

	GError *e = NULL;
	Config *c = config_load(path, &e);
	g_assert_no_error(e);
	g_assert_nonnull(c);

	char *r = log_redact("rtsp://tester:zz/yy@127.0.0.1:1/live");
	g_assert_null(strstr(r, "zz/yy"));
	g_assert_null(strstr(captured->str, "zz/yy")); /* the load warning must not echo it */
	g_free(r);
	config_free(c);
	g_unlink(path);
	g_free(path);
	g_rmdir(dir);
	g_free(dir);
	teardown();
}

static void test_levels(void)
{
	setup();
	log_set_level(LOG_LVL_WARNING);
	log_info("hidden");
	log_warn("shown");
	g_assert_null(strstr(captured->str, "hidden"));
	g_assert_nonnull(strstr(captured->str, "shown"));
	LogLevel l;
	g_assert_true(log_level_from_string("Debug", &l));
	g_assert_cmpint(l, ==, LOG_LVL_DEBUG);
	g_assert_false(log_level_from_string("loud", &l));
	teardown();
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/logging/url-userinfo", test_url_userinfo);
	g_test_add_func("/logging/registered-secret-forms", test_registered_secret_forms);
	g_test_add_func("/logging/log-msg-redacts", test_log_msg_redacts);
	g_test_add_func("/logging/redaction-hard-cases", test_redaction_hard_cases);
	g_test_add_func("/logging/url-override-secrets", test_url_override_secrets_registered);
	g_test_add_func("/logging/levels", test_levels);
	return g_test_run();
}
