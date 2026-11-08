#include "logging.h"

#include <glib.h>

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
	g_test_add_func("/logging/levels", test_levels);
	return g_test_run();
}
