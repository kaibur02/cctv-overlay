#define _GNU_SOURCE
#include "config.h"

#include "util.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>

G_DEFINE_QUARK(cctv-config-error-quark, config_error)

#define FAIL(err, path, ...) config_fail((err), (path), __VA_ARGS__)

static gboolean config_fail(GError **err, const char *path, const char *fmt, ...) G_GNUC_PRINTF(3, 4);

static gboolean config_fail(GError **err, const char *path, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	char *msg = g_strdup_vprintf(fmt, ap);
	va_end(ap);
	g_set_error(err, CONFIG_ERROR, 1, "%s: %s", path, msg);
	g_free(msg);
	return FALSE;
}

char *config_default_path(void)
{
	return g_build_filename(g_get_user_config_dir(), "cctv-overlay", "config.ini", NULL);
}

/* ---- typed getters (raw values: no escape processing, no inline comments) ---- */

static gboolean has(GKeyFile *kf, const char *g, const char *k)
{
	return g_key_file_has_key(kf, g, k, NULL);
}

static gboolean get_str(GKeyFile *kf, const char *path, const char *g, const char *k, char **out,
			gboolean allow_empty, GError **err)
{
	if (!has(kf, g, k))
		return TRUE;
	char *v = g_key_file_get_value(kf, g, k, NULL);
	if (!v || (!*v && !allow_empty)) {
		g_free(v);
		return FAIL(err, path, "[%s] %s: must not be empty", g, k);
	}
	g_free(*out);
	*out = v;
	return TRUE;
}

static gboolean get_int(GKeyFile *kf, const char *path, const char *g, const char *k, int min, int max,
			int *out, GError **err)
{
	if (!has(kf, g, k))
		return TRUE;
	char *raw = g_key_file_get_value(kf, g, k, NULL);
	char *end = NULL;
	errno = 0;
	long long v = raw ? g_ascii_strtoll(raw, &end, 10) : 0;
	gboolean bad = !raw || !*raw || *end != '\0' || errno != 0;
	g_free(raw);
	if (bad)
		return FAIL(err, path, "[%s] %s: must be an integer between %d and %d (no trailing comments)",
			    g, k, min, max);
	if (v < min || v > max)
		return FAIL(err, path, "[%s] %s: %lld is out of range (%d..%d)", g, k, v, min, max);
	*out = (int)v;
	return TRUE;
}

static gboolean get_bool(GKeyFile *kf, const char *path, const char *g, const char *k, gboolean *out,
			 GError **err)
{
	if (!has(kf, g, k))
		return TRUE;
	GError *e = NULL;
	gboolean v = g_key_file_get_boolean(kf, g, k, &e);
	if (e) {
		g_error_free(e);
		return FAIL(err, path, "[%s] %s: must be true or false", g, k);
	}
	*out = v;
	return TRUE;
}

static gboolean get_enum(GKeyFile *kf, const char *path, const char *g, const char *k,
			 const char *const *allowed, char **out, GError **err)
{
	if (!has(kf, g, k))
		return TRUE;
	char *v = g_key_file_get_value(kf, g, k, NULL);
	for (guint i = 0; v && allowed[i]; i++) {
		if (strcmp(v, allowed[i]) == 0) {
			g_free(*out);
			*out = v;
			return TRUE;
		}
	}
	char *list = g_strjoinv(" | ", (char **)allowed);
	gboolean r = FAIL(err, path, "[%s] %s: '%s' is not valid; use one of: %s", g, k, v ? v : "", list);
	g_free(list);
	g_free(v);
	return r;
}

static gboolean check_keys(GKeyFile *kf, const char *path, const char *g, const char *const *allowed,
			   GError **err)
{
	gsize n = 0;
	char **keys = g_key_file_get_keys(kf, g, &n, NULL);
	gboolean ok = TRUE;

	for (gsize i = 0; keys && i < n && ok; i++) {
		gboolean known = FALSE;
		for (guint j = 0; allowed[j]; j++)
			if (strcmp(keys[i], allowed[j]) == 0)
				known = TRUE;
		if (known)
			continue;
		if (strcmp(keys[i], "password") == 0)
			ok = FAIL(err, path,
				  "[%s] password: passwords are not allowed in config.ini; put it in secrets.ini "
				  "(mode 0600) or set CCTV_OVERLAY_PASSWORD",
				  g);
		else
			ok = FAIL(err, path, "[%s] unknown key '%s'", g, keys[i]);
	}
	g_strfreev(keys);
	return ok;
}

static const char *const ANCHORS[] = { "top-left", "top-right", "bottom-left", "bottom-right", NULL };
static const char *const LAYERS[] = { "overlay", "top", NULL };
static const char *const TRANSPORTS[] = { "tcp", "udp", NULL };
static const char *const HWDECS[] = { "vaapi", "vaapi-copy", "auto-safe", "no", NULL };

static const char *const K_GENERAL[] = { "log_level", "monitor", "start_hidden", NULL };
static const char *const K_OVERLAY[] = { "width", "height", "anchor", "margin_x", "margin_y", "layer",
					 "show_label", "show_status", "opacity", NULL };
static const char *const K_STREAM[] = { "transport", "subtype", "hwdec", "preview_snapshot", "preload_next",
					"reconnect", NULL };
static const char *const K_RECORDER[] = { "host", "port", "username", "path", NULL };
static const char *const K_CAMERA[] = { "name", "channel", "subtype", "url", NULL };

static void camera_free(Camera *c)
{
	if (!c)
		return;
	g_free(c->id);
	g_free(c->name);
	util_wipe_free(c->url);
	g_free(c);
}

void config_free(Config *cfg)
{
	if (!cfg)
		return;
	g_free(cfg->config_path);
	g_free(cfg->secrets_path);
	g_free(cfg->monitor);
	g_free(cfg->anchor);
	g_free(cfg->layer);
	g_free(cfg->transport);
	g_free(cfg->hwdec);
	g_free(cfg->host);
	g_free(cfg->path);
	g_free(cfg->username);
	util_wipe_free(cfg->password);
	if (cfg->cameras)
		g_ptr_array_free(cfg->cameras, TRUE);
	g_free(cfg);
}

static gboolean host_ok(const char *h)
{
	for (; *h; h++)
		if (g_ascii_isspace(*h) || strchr("/@?#\\", *h))
			return FALSE;
	return TRUE;
}

static gboolean load_cameras(GKeyFile *kf, Config *cfg, GError **err)
{
	const char *path = cfg->config_path;
	gsize ng = 0;
	char **groups = g_key_file_get_groups(kf, &ng);
	gboolean ok = TRUE;

	for (gsize i = 0; i < ng && ok; i++) {
		const char *g = groups[i];
		if (!g_str_has_prefix(g, "camera."))
			continue;
		const char *id = g + strlen("camera.");
		if (!*id) {
			ok = FAIL(err, path, "[%s]: camera section needs an id, e.g. [camera.front]", g);
			break;
		}
		ok = check_keys(kf, path, g, K_CAMERA, err);
		if (!ok)
			break;

		Camera *c = g_new0(Camera, 1);
		c->id = g_strdup(id);
		c->subtype = -1;
		g_ptr_array_add(cfg->cameras, c);

		ok = get_str(kf, path, g, "name", &c->name, FALSE, err) &&
		     get_str(kf, path, g, "url", &c->url, FALSE, err) &&
		     get_int(kf, path, g, "channel", 1, 1024, &c->channel, err) &&
		     get_int(kf, path, g, "subtype", 0, 3, &c->subtype, err);
		if (!ok)
			break;
		if (!c->name) {
			ok = FAIL(err, path, "[%s] name: required (the label shown on the overlay)", g);
			break;
		}
		if (c->url) {
			if (!g_str_has_prefix(c->url, "rtsp://") && !g_str_has_prefix(c->url, "rtsps://"))
				ok = FAIL(err, path, "[%s] url: must start with rtsp:// or rtsps://", g);
		} else if (c->channel < 1) {
			ok = FAIL(err, path, "[%s] channel: required (>= 1) unless 'url' is set", g);
		}
		if (!ok)
			break;
		for (guint j = 0; j + 1 < cfg->cameras->len; j++) {
			const Camera *o = g_ptr_array_index(cfg->cameras, j);
			if (g_ascii_strcasecmp(o->name, c->name) == 0) {
				ok = FAIL(err, path, "[%s] name: '%s' duplicates [camera.%s]; names must be unique",
					  g, c->name, o->id);
				break;
			}
		}
	}
	g_strfreev(groups);
	if (ok && cfg->cameras->len == 0)
		return FAIL(err, path, "no cameras defined; add at least one [camera.<id>] section with name and channel");
	return ok;
}

static void load_secrets(Config *cfg)
{
	const char *u = g_getenv("CCTV_OVERLAY_USERNAME");
	const char *p = g_getenv("CCTV_OVERLAY_PASSWORD");
	char *dir = g_path_get_dirname(cfg->config_path);

	cfg->secrets_path = g_build_filename(dir, "secrets.ini", NULL);
	g_free(dir);

	GStatBuf st;
	if (g_stat(cfg->secrets_path, &st) == 0) {
		if (st.st_mode & 077)
			log_warn("secrets file %s has mode %04o (readable by group/other); run: chmod 600 %s",
				 cfg->secrets_path, (unsigned)(st.st_mode & 07777), cfg->secrets_path);
		GKeyFile *kf = g_key_file_new();
		GError *e = NULL;
		if (!g_key_file_load_from_file(kf, cfg->secrets_path, G_KEY_FILE_NONE, &e)) {
			/* The parser message can quote the offending line, which may be a password. */
			log_warn("secrets file %s could not be parsed (details withheld); expected a [recorder] "
				 "section with 'password = ...'",
				 cfg->secrets_path);
			g_clear_error(&e);
		} else {
			char *fp = g_key_file_get_value(kf, "recorder", "password", NULL);
			char *fu = g_key_file_get_value(kf, "recorder", "username", NULL);
			if (fp && *fp) {
				util_wipe_free(cfg->password);
				cfg->password = fp;
			} else {
				util_wipe_free(fp);
			}
			if (fu && *fu) {
				g_free(cfg->username);
				cfg->username = fu;
			} else {
				g_free(fu);
			}
		}
		if (e)
			g_error_free(e);
		g_key_file_free(kf); /* NB: GKeyFile does not wipe; accepted, see docs/behaviour.md */
	}
	if (u && *u) {
		g_free(cfg->username);
		cfg->username = g_strdup(u);
	}
	if (p && *p) {
		util_wipe_free(cfg->password);
		cfg->password = g_strdup(p);
	}
	log_register_secret(cfg->password);
}

Config *config_load(const char *path, GError **err)
{
	Config *cfg = g_new0(Config, 1);
	GKeyFile *kf = g_key_file_new();
	GError *e = NULL;

	cfg->config_path = path ? g_strdup(path) : config_default_path();
	cfg->log_level = LOG_LVL_INFO;
	cfg->monitor = g_strdup("focused");
	cfg->start_hidden = TRUE;
	cfg->width = 480;
	cfg->height = 270;
	cfg->anchor = g_strdup("top-right");
	cfg->margin_x = cfg->margin_y = 24;
	cfg->layer = g_strdup("overlay");
	cfg->show_label = cfg->show_status = TRUE;
	cfg->opacity = 1.0;
	cfg->transport = g_strdup("tcp");
	cfg->subtype = 1;
	cfg->hwdec = g_strdup("vaapi");
	cfg->preview_snapshot = cfg->reconnect = TRUE;
	cfg->port = 554;
	cfg->path = g_strdup("/cam/realmonitor");
	cfg->username = g_strdup("");
	cfg->cameras = g_ptr_array_new_with_free_func((GDestroyNotify)camera_free);

	const char *p = cfg->config_path;
	gboolean ok = TRUE;

	if (!g_key_file_load_from_file(kf, p, G_KEY_FILE_NONE, &e)) {
		if (g_error_matches(e, G_FILE_ERROR, G_FILE_ERROR_NOENT))
			ok = FAIL(err, p, "config file not found; copy examples/config.example.ini there "
				  "(see docs/behaviour.md)");
		else
			ok = FAIL(err, p, "%s", e->message);
		g_clear_error(&e);
		goto out;
	}

	/* unknown groups / keys */
	gsize ng = 0;
	char **groups = g_key_file_get_groups(kf, &ng);
	for (gsize i = 0; i < ng && ok; i++) {
		const char *g = groups[i];
		if (strcmp(g, "general") == 0) ok = check_keys(kf, p, g, K_GENERAL, err);
		else if (strcmp(g, "overlay") == 0) ok = check_keys(kf, p, g, K_OVERLAY, err);
		else if (strcmp(g, "stream") == 0) ok = check_keys(kf, p, g, K_STREAM, err);
		else if (strcmp(g, "recorder") == 0) ok = check_keys(kf, p, g, K_RECORDER, err);
		else if (!g_str_has_prefix(g, "camera."))
			ok = FAIL(err, p, "unknown section [%s]; expected general, overlay, stream, recorder or camera.<id>", g);
	}
	g_strfreev(groups);
	if (!ok)
		goto out;

	char *ll = NULL;
	if (has(kf, "general", "log_level")) {
		ll = g_key_file_get_value(kf, "general", "log_level", NULL);
		if (!ll || !log_level_from_string(ll, &cfg->log_level)) {
			ok = FAIL(err, p, "[general] log_level: '%s' is not valid; use debug | info | warning | error",
				  ll ? ll : "");
			g_free(ll);
			goto out;
		}
		g_free(ll);
	}

	ok = get_str(kf, p, "general", "monitor", &cfg->monitor, FALSE, err) &&
	     get_bool(kf, p, "general", "start_hidden", &cfg->start_hidden, err) &&
	     get_int(kf, p, "overlay", "width", 64, 7680, &cfg->width, err) &&
	     get_int(kf, p, "overlay", "height", 64, 4320, &cfg->height, err) &&
	     get_enum(kf, p, "overlay", "anchor", ANCHORS, &cfg->anchor, err) &&
	     get_int(kf, p, "overlay", "margin_x", 0, 10000, &cfg->margin_x, err) &&
	     get_int(kf, p, "overlay", "margin_y", 0, 10000, &cfg->margin_y, err) &&
	     get_enum(kf, p, "overlay", "layer", LAYERS, &cfg->layer, err) &&
	     get_bool(kf, p, "overlay", "show_label", &cfg->show_label, err) &&
	     get_bool(kf, p, "overlay", "show_status", &cfg->show_status, err) &&
	     get_enum(kf, p, "stream", "transport", TRANSPORTS, &cfg->transport, err) &&
	     get_int(kf, p, "stream", "subtype", 0, 3, &cfg->subtype, err) &&
	     get_enum(kf, p, "stream", "hwdec", HWDECS, &cfg->hwdec, err) &&
	     get_bool(kf, p, "stream", "preview_snapshot", &cfg->preview_snapshot, err) &&
	     get_bool(kf, p, "stream", "preload_next", &cfg->preload_next, err) &&
	     get_bool(kf, p, "stream", "reconnect", &cfg->reconnect, err) &&
	     get_str(kf, p, "recorder", "host", &cfg->host, FALSE, err) &&
	     get_int(kf, p, "recorder", "port", 1, 65535, &cfg->port, err) &&
	     get_str(kf, p, "recorder", "path", &cfg->path, FALSE, err) &&
	     get_str(kf, p, "recorder", "username", &cfg->username, TRUE, err);
	if (!ok)
		goto out;

	if (has(kf, "overlay", "opacity")) {
		char *raw = g_key_file_get_value(kf, "overlay", "opacity", NULL);
		char *end = NULL;
		double o = raw ? g_ascii_strtod(raw, &end) : 0;
		gboolean bad = !raw || !*raw || *end != '\0';
		g_free(raw);
		if (bad) {
			ok = FAIL(err, p, "[overlay] opacity: must be a number between 0.1 and 1.0");
			goto out;
		}
		if (o < 0.1 || o > 1.0) {
			ok = FAIL(err, p, "[overlay] opacity: %g is out of range (0.1..1.0)", o);
			goto out;
		}
		cfg->opacity = o;
	}
	if (cfg->path[0] != '/') {
		ok = FAIL(err, p, "[recorder] path: must start with '/' (got '%s')", cfg->path);
		goto out;
	}
	if (cfg->host && !host_ok(cfg->host)) {
		ok = FAIL(err, p, "[recorder] host: '%s' is not a valid host name or address", cfg->host);
		goto out;
	}

	ok = load_cameras(kf, cfg, err);
	if (!ok)
		goto out;

	for (guint i = 0; i < cfg->cameras->len; i++) {
		const Camera *c = g_ptr_array_index(cfg->cameras, i);
		if (!c->url && !cfg->host) {
			ok = FAIL(err, p, "[recorder] host: required because [camera.%s] has no 'url' "
				  "(set host to your recorder's address)", c->id);
			goto out;
		}
	}

	load_secrets(cfg);

out:
	g_key_file_free(kf);
	if (!ok) {
		config_free(cfg);
		return NULL;
	}
	return cfg;
}
