#include "camera.h"
#include "testlib.h"
#include "util.h"

#include <string.h>

#define PW "p@ss:w/rd%1"

static Config *load_with(const char *text, const char *password)
{
	char *dir;
	char *path = tl_write_config(text, NULL, 0, &dir);
	g_unsetenv("CCTV_OVERLAY_PASSWORD");
	g_unsetenv("CCTV_OVERLAY_USERNAME");
	if (password)
		g_setenv("CCTV_OVERLAY_PASSWORD", password, TRUE);
	GError *e = NULL;
	Config *c = config_load(path, &e);
	g_assert_no_error(e);
	g_unsetenv("CCTV_OVERLAY_PASSWORD");
	g_free(path);
	tl_rm_dir(dir);
	return c;
}

static void test_cycle(void)
{
	Config *c = load_with(TL_GOOD_CONFIG, NULL);
	CameraList *l = camera_list_new(c);
	g_assert_cmpuint(camera_list_count(l), ==, 3);
	g_assert_cmpuint(camera_list_current(l), ==, 0);
	g_assert_cmpuint(camera_list_next(l), ==, 1);
	g_assert_cmpuint(camera_list_next(l), ==, 2);
	g_assert_cmpuint(camera_list_next(l), ==, 0); /* wrap */
	g_assert_cmpuint(camera_list_prev(l), ==, 2); /* wrap back */
	g_assert_cmpuint(camera_list_prev(l), ==, 1);
	g_assert_cmpstr(camera_list_current_camera(l)->name, ==, "Driveway");
	camera_list_set_current(l, 99); /* ignored */
	g_assert_cmpuint(camera_list_current(l), ==, 1);
	camera_list_free(l);
	config_free(c);
}

static void test_find(void)
{
	Config *c = load_with(TL_GOOD_CONFIG, NULL);
	CameraList *l = camera_list_new(c);
	guint i = 99;
	g_assert_true(camera_list_find(l, "Driveway", &i));
	g_assert_cmpuint(i, ==, 1);
	g_assert_true(camera_list_find(l, "garden", &i)); /* name, case-insensitive */
	g_assert_cmpuint(i, ==, 2);
	g_assert_true(camera_list_find(l, "front", &i)); /* id */
	g_assert_cmpuint(i, ==, 0);
	g_assert_true(camera_list_find(l, "2", &i)); /* zero-based index */
	g_assert_cmpuint(i, ==, 2);
	g_assert_false(camera_list_find(l, "3", &i));
	g_assert_false(camera_list_find(l, "nope", &i));
	g_assert_false(camera_list_find(l, "", &i));
	g_assert_false(camera_list_find(l, "-1", &i));
	camera_list_free(l);
	config_free(c);
}

static void test_url_encoding(void)
{
	Config *c = load_with(TL_GOOD_CONFIG, PW);
	GError *e = NULL;
	char *url = camera_resolve_url(c, g_ptr_array_index(c->cameras, 0), &e);
	g_assert_no_error(e);
	g_assert_cmpstr(url, ==,
			"rtsp://tester:p%40ss%3Aw%2Frd%251@192.168.1.100:554/cam/realmonitor?channel=1&subtype=1");
	util_wipe_free(url);

	/* per-camera subtype override */
	url = camera_resolve_url(c, g_ptr_array_index(c->cameras, 1), &e);
	g_assert_nonnull(strstr(url, "?channel=2&subtype=0"));
	util_wipe_free(url);
	config_free(c);
}

static void test_url_userinfo_awkward_username(void)
{
	Config *c = load_with("[recorder]\nhost = h.example.invalid\nusername = us er@x\n"
			      "[camera.a]\nname = A\nchannel = 1\n", "a b?c#d");
	char *url = camera_resolve_url(c, g_ptr_array_index(c->cameras, 0), NULL);
	g_assert_true(g_str_has_prefix(url, "rtsp://us%20er%40x:a%20b%3Fc%23d@h.example.invalid:554/"));
	util_wipe_free(url);
	config_free(c);
}

static void test_ipv6_host(void)
{
	Config *c = load_with("[recorder]\nhost = fe80::1\nusername = u\n[camera.a]\nname = A\nchannel = 1\n", "p");
	char *url = camera_resolve_url(c, g_ptr_array_index(c->cameras, 0), NULL);
	g_assert_nonnull(strstr(url, "@[fe80::1]:554/"));
	util_wipe_free(url);
	config_free(c);
}

static void test_missing_credentials_errors(void)
{
	Config *c = load_with(TL_GOOD_CONFIG, NULL); /* no password */
	GError *e = NULL;
	g_assert_null(camera_resolve_url(c, g_ptr_array_index(c->cameras, 0), &e));
	g_assert_nonnull(e);
	g_assert_nonnull(strstr(e->message, "password"));
	g_assert_null(strstr(e->message, "rtsp://"));
	g_clear_error(&e);
	config_free(c);

	c = load_with("[recorder]\nhost = h\n[camera.a]\nname = A\nchannel = 1\n", "pw");
	g_assert_null(camera_resolve_url(c, g_ptr_array_index(c->cameras, 0), &e));
	g_assert_nonnull(strstr(e->message, "username"));
	g_error_free(e);
	config_free(c);
}

static void test_override_url(void)
{
	Config *c = load_with("[camera.x]\nname = X\nurl = rtsp://h.example.invalid/s\n", NULL);
	char *url = camera_resolve_url(c, g_ptr_array_index(c->cameras, 0), NULL);
	g_assert_cmpstr(url, ==, "rtsp://h.example.invalid/s"); /* no credentials needed */
	util_wipe_free(url);
	config_free(c);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/camera/cycle-wrap", test_cycle);
	g_test_add_func("/camera/find", test_find);
	g_test_add_func("/camera/url-encoding", test_url_encoding);
	g_test_add_func("/camera/url-awkward-username", test_url_userinfo_awkward_username);
	g_test_add_func("/camera/ipv6-host", test_ipv6_host);
	g_test_add_func("/camera/missing-credentials", test_missing_credentials_errors);
	g_test_add_func("/camera/override-url", test_override_url);
	return g_test_run();
}
