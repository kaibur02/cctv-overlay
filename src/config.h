#ifndef CCTV_CONFIG_H
#define CCTV_CONFIG_H

#include "logging.h"

#include <glib.h>

#define CONFIG_ERROR (config_error_quark())
GQuark config_error_quark(void);

typedef struct {
	char *id;      /* group suffix: [camera.<id>] */
	char *name;    /* display name, unique (case-insensitive) */
	int channel;   /* >= 1; 0 when only a url override is used */
	int subtype;   /* -1 = use [stream] subtype */
	char *url;     /* optional full RTSP URL override (discouraged: may contain a secret) */
} Camera;

typedef struct {
	char *config_path;
	char *secrets_path;

	/* [general] */
	LogLevel log_level;
	char *monitor;
	gboolean start_hidden;

	/* [overlay] */
	int width, height;
	char *anchor; /* top-left|top-right|bottom-left|bottom-right */
	int margin_x, margin_y;
	char *layer;  /* overlay|top */
	gboolean show_label, show_status;
	double opacity;

	/* [stream] */
	char *transport; /* tcp|udp */
	int subtype;
	char *hwdec;     /* vaapi|vaapi-copy|auto-safe|no */
	gboolean preview_snapshot, preload_next, reconnect;

	/* [recorder] */
	char *host; /* may be NULL only if every camera has a url override */
	int port;
	char *path;
	char *username; /* effective: env > secrets.ini > config.ini; may be empty */

	/* Secret. Never print, log or copy this; only camera_resolve_url() reads it. */
	char *password; /* NULL when not configured */

	GPtrArray *cameras; /* Camera*, file order */
} Config;

/* Default: $XDG_CONFIG_HOME/cctv-overlay/config.ini (or ~/.config/...). */
char *config_default_path(void);

/* Loads and strictly validates. path == NULL -> default path. Only the first problem is reported.
 * A missing password is NOT an error here. Warns when secrets.ini is group/other accessible. */
Config *config_load(const char *path, GError **err);

void config_free(Config *cfg);

#endif
