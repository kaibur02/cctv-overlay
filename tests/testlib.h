#ifndef TESTLIB_H
#define TESTLIB_H

#include <glib.h>
#include <glib/gstdio.h>

/* Write a config (and optionally secrets) into a fresh temp dir; returns the config path. */
static inline char *tl_write_config(const char *config, const char *secrets, int secrets_mode, char **dir_out)
{
	GError *e = NULL;
	char *dir = g_dir_make_tmp("cctv-test-XXXXXX", &e);
	g_assert_no_error(e);
	char *cfg = g_build_filename(dir, "config.ini", NULL);
	g_assert_true(g_file_set_contents(cfg, config, -1, NULL));
	if (secrets) {
		char *sp = g_build_filename(dir, "secrets.ini", NULL);
		g_assert_true(g_file_set_contents(sp, secrets, -1, NULL));
		g_chmod(sp, secrets_mode);
		g_free(sp);
	}
	if (dir_out)
		*dir_out = dir;
	else
		g_free(dir);
	return cfg;
}

static inline void tl_rm_dir(char *dir)
{
	char *cmd = g_strdup_printf("rm -rf '%s'", dir);
	g_assert_cmpint(system(cmd), ==, 0);
	g_free(cmd);
	g_free(dir);
}

#define TL_GOOD_CONFIG                                                                                    \
	"[recorder]\nhost = 192.168.1.100\nusername = tester\n"                                          \
	"[camera.front]\nname = Front Door\nchannel = 1\n"                                               \
	"[camera.driveway]\nname = Driveway\nchannel = 2\nsubtype = 0\n"                                  \
	"[camera.garden]\nname = Garden\nchannel = 3\n"

#endif
