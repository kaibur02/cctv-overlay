#ifndef CCTV_UTIL_H
#define CCTV_UTIL_H

#include <glib.h>

#define CCTV_VERSION "0.1.0"

/* RFC 3986 percent-encoding: everything except unreserved (A-Za-z0-9-._~) is encoded. */
char *util_pct_encode(const char *s);

/* Overwrite memory in a way the compiler may not optimise away. */
void util_wipe(void *p, gsize n);

/* Wipe the whole NUL-terminated string (including its terminator) and free it. NULL-safe. */
void util_wipe_free(char *s);

gint64 util_monotonic_ms(void);

#endif
