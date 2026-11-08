#define _GNU_SOURCE
#include "logging.h"

#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static LogLevel g_level = LOG_LVL_INFO;
static GPtrArray *g_secrets; /* char*, sorted by descending length */
static LogSink g_sink;
static void *g_sink_user;
static GRegex *g_url_re;

void log_set_level(LogLevel level) { g_level = level; }
LogLevel log_get_level(void) { return g_level; }

gboolean log_level_from_string(const char *s, LogLevel *out)
{
	static const struct { const char *n; LogLevel l; } tbl[] = {
		{ "debug", LOG_LVL_DEBUG }, { "info", LOG_LVL_INFO },
		{ "warning", LOG_LVL_WARNING }, { "warn", LOG_LVL_WARNING },
		{ "error", LOG_LVL_ERROR },
	};
	for (guint i = 0; i < G_N_ELEMENTS(tbl); i++) {
		if (g_ascii_strcasecmp(s, tbl[i].n) == 0) {
			*out = tbl[i].l;
			return TRUE;
		}
	}
	return FALSE;
}

static void add_secret_form(const char *form)
{
	if (!form || !*form)
		return;
	for (guint i = 0; i < g_secrets->len; i++)
		if (strcmp(g_ptr_array_index(g_secrets, i), form) == 0)
			return;
	guint pos = 0;
	while (pos < g_secrets->len && strlen(g_ptr_array_index(g_secrets, pos)) >= strlen(form))
		pos++;
	g_ptr_array_insert(g_secrets, (gint)pos, g_strdup(form));
}

void log_register_secret(const char *secret)
{
	if (!secret || !*secret)
		return;
	if (!g_secrets)
		g_secrets = g_ptr_array_new_with_free_func((GDestroyNotify)util_wipe_free);
	add_secret_form(secret);
	char *enc = util_pct_encode(secret);
	add_secret_form(enc);
	g_free(enc);
}

void log_clear_secrets(void)
{
	if (g_secrets)
		g_ptr_array_set_size(g_secrets, 0);
}

static char *replace_all(const char *text, const char *needle, const char *repl)
{
	GString *out = g_string_new(NULL);
	size_t nl = strlen(needle);
	const char *p = text, *hit;

	while ((hit = strstr(p, needle)) != NULL) {
		g_string_append_len(out, p, hit - p);
		g_string_append(out, repl);
		p = hit + nl;
	}
	g_string_append(out, p);
	return g_string_free(out, FALSE);
}

char *log_redact(const char *text)
{
	char *cur = g_strdup(text ? text : "");

	if (g_secrets) {
		for (guint i = 0; i < g_secrets->len; i++) {
			char *next = replace_all(cur, g_ptr_array_index(g_secrets, i), "***");
			g_free(cur);
			cur = next;
		}
	}
	if (!g_url_re)
		g_url_re = g_regex_new("([A-Za-z][A-Za-z0-9+.\\-]*://)[^/\\s\"']*@", 0, 0, NULL);
	if (g_url_re) {
		char *next = g_regex_replace(g_url_re, cur, -1, 0, "\\1***:***@", 0, NULL);
		if (next) {
			g_free(cur);
			cur = next;
		}
	}
	return cur;
}

void log_set_sink(LogSink sink, void *user)
{
	g_sink = sink;
	g_sink_user = user;
}

void log_msg(LogLevel level, const char *fmt, ...)
{
	static const char *const names[] = { "debug", "info", "warning", "error" };

	if (level < g_level)
		return;

	va_list ap;
	va_start(ap, fmt);
	char *raw = g_strdup_vprintf(fmt, ap);
	va_end(ap);

	char *safe = log_redact(raw);
	util_wipe_free(raw);

	if (g_sink) {
		g_sink(level, safe, g_sink_user);
	} else {
		fprintf(stderr, "cctv-overlay: %s: %s\n", names[level], safe);
		fflush(stderr);
	}
	g_free(safe);
}
