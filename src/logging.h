#ifndef CCTV_LOGGING_H
#define CCTV_LOGGING_H

#include <glib.h>

typedef enum { LOG_LVL_DEBUG = 0, LOG_LVL_INFO, LOG_LVL_WARNING, LOG_LVL_ERROR } LogLevel;

void log_set_level(LogLevel level);
LogLevel log_get_level(void);
gboolean log_level_from_string(const char *s, LogLevel *out);

/* Register a secret. The value and its RFC 3986 percent-encoded form are replaced by "***"
 * in every message that passes through log_redact()/log_msg(). Empty strings are ignored. */
void log_register_secret(const char *secret);
void log_clear_secrets(void);

/* Returns a newly allocated redacted copy: registered secrets -> "***" and any
 * "scheme-colon-slash-slash userinfo @" becomes "scheme://" followed by "***:***@". */
char *log_redact(const char *text);

/* Test hook: when set, redacted lines are delivered here instead of stderr. */
typedef void (*LogSink)(LogLevel level, const char *line, void *user);
void log_set_sink(LogSink sink, void *user);

void log_msg(LogLevel level, const char *fmt, ...) G_GNUC_PRINTF(2, 3);

#define log_debug(...) log_msg(LOG_LVL_DEBUG, __VA_ARGS__)
#define log_info(...) log_msg(LOG_LVL_INFO, __VA_ARGS__)
#define log_warn(...) log_msg(LOG_LVL_WARNING, __VA_ARGS__)
#define log_error(...) log_msg(LOG_LVL_ERROR, __VA_ARGS__)

#endif
