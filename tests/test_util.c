#include "util.h"

#include <glib.h>

static void check_enc(const char *in, const char *want)
{
	char *got = util_pct_encode(in);
	g_assert_cmpstr(got, ==, want);
	g_free(got);
}

static void test_pct(void)
{
	check_enc("", "");
	check_enc("abcXYZ019-._~", "abcXYZ019-._~");
	check_enc("p@ss:w/rd%1", "p%40ss%3Aw%2Frd%251");
	check_enc("a b?c#d", "a%20b%3Fc%23d");
	check_enc("\xc3\xa9", "%C3%A9");
	check_enc("!*'();=+$,[]", "%21%2A%27%28%29%3B%3D%2B%24%2C%5B%5D");
}

static void test_wipe(void)
{
	char buf[8] = "secret";
	util_wipe(buf, sizeof buf);
	for (guint i = 0; i < sizeof buf; i++)
		g_assert_cmpint(buf[i], ==, 0);
	util_wipe_free(NULL);
	util_wipe_free(g_strdup("x"));
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/util/pct-encode", test_pct);
	g_test_add_func("/util/wipe", test_wipe);
	return g_test_run();
}
