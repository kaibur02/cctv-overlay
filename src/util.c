#define _GNU_SOURCE
#include "util.h"

#include <string.h>

char *util_pct_encode(const char *s)
{
	static const char hex[] = "0123456789ABCDEF";
	GString *out = g_string_sized_new(strlen(s) * 3 + 1);

	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		if (g_ascii_isalnum(*p) || *p == '-' || *p == '.' || *p == '_' || *p == '~') {
			g_string_append_c(out, (char)*p);
		} else {
			g_string_append_c(out, '%');
			g_string_append_c(out, hex[*p >> 4]);
			g_string_append_c(out, hex[*p & 15]);
		}
	}
	return g_string_free(out, FALSE);
}

void util_wipe(void *p, gsize n)
{
	if (p && n)
		explicit_bzero(p, n);
}

void util_wipe_free(char *s)
{
	if (!s)
		return;
	util_wipe(s, strlen(s) + 1);
	g_free(s);
}

gint64 util_monotonic_ms(void)
{
	return g_get_monotonic_time() / 1000;
}
