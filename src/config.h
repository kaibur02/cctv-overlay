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
	char *host;    /* optional per-camera host (standalone IP cameras); NULL = [recorder] host */
	int port;      /* 0 = [recorder] port */
	char *path;    /* optional per-camera path/template; NULL = [recorder] path */
} Camera;

#define CONFIG_GRID_MAX 4

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
	gboolean audio;  /* initial audio state (toggled at runtime with `cctv-overlay audio`) */

	/* [grid] */
	gboolean grid_enabled;
	int grid_subtype;              /* -1 = per camera / [stream] subtype */
	guint grid_count;              /* resolved: 0 = no grid view (disabled or fewer than 2 cameras) */
	guint grid_idx[CONFIG_GRID_MAX]; /* camera indexes shown in the grid, in tile order */

	/* [recorder] */
	char *host; /* may be NULL only if every camera has a url override */
	int port;
	char *path;     /* plain path (Dahua-style ?channel=&subtype= is appended) or a template with {placeholders} */
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

/* Expand a [recorder]/camera path. With dahua_query and no '{' in path, the Dahua query "?channel=N&subtype=S" is appended.
 * Placeholders: {channel} {channel02} {subtype} (0 main, 1 sub) {stream} (subtype+1) {mainsub} ("main"/"sub").
 * Returns NULL with *err set for an unknown or unterminated placeholder. */
char *config_expand_path(const char *path, gboolean dahua_query, int channel, int subtype, GError **err);

#endif
