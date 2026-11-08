#define _GNU_SOURCE
#include "camera.h"

#include "util.h"

#include <stdlib.h>
#include <string.h>

struct CameraList {
	const Config *cfg;
	guint cur;
};

CameraList *camera_list_new(const Config *cfg)
{
	CameraList *l = g_new0(CameraList, 1);
	l->cfg = cfg;
	return l;
}

void camera_list_free(CameraList *l) { g_free(l); }

guint camera_list_count(const CameraList *l) { return l->cfg->cameras->len; }

const Camera *camera_list_get(const CameraList *l, guint i)
{
	return i < l->cfg->cameras->len ? g_ptr_array_index(l->cfg->cameras, i) : NULL;
}

guint camera_list_current(const CameraList *l) { return l->cur; }
const Camera *camera_list_current_camera(const CameraList *l) { return camera_list_get(l, l->cur); }

void camera_list_set_current(CameraList *l, guint i)
{
	if (i < camera_list_count(l))
		l->cur = i;
}

guint camera_list_next(CameraList *l)
{
	l->cur = (l->cur + 1) % camera_list_count(l);
	return l->cur;
}

guint camera_list_prev(CameraList *l)
{
	guint n = camera_list_count(l);
	l->cur = (l->cur + n - 1) % n;
	return l->cur;
}

gboolean camera_list_find_id(const CameraList *l, const char *id, guint *index)
{
	for (guint i = 0; i < camera_list_count(l); i++) {
		if (strcmp(camera_list_get(l, i)->id, id) == 0) {
			*index = i;
			return TRUE;
		}
	}
	return FALSE;
}

gboolean camera_list_find(const CameraList *l, const char *spec, guint *index)
{
	for (guint i = 0; i < camera_list_count(l); i++) {
		if (g_ascii_strcasecmp(camera_list_get(l, i)->name, spec) == 0) {
			*index = i;
			return TRUE;
		}
	}
	if (camera_list_find_id(l, spec, index))
		return TRUE;
	if (*spec && strspn(spec, "0123456789") == strlen(spec) && strlen(spec) < 6) {
		unsigned long v = strtoul(spec, NULL, 10);
		if (v < camera_list_count(l)) {
			*index = (guint)v;
			return TRUE;
		}
	}
	return FALSE;
}

char *camera_resolve_url(const Config *cfg, const Camera *cam, int subtype_override, GError **err)
{
	if (cam->url)
		return g_strdup(cam->url);

	const char *host = cam->host ? cam->host : cfg->host;
	int port = cam->port > 0 ? cam->port : cfg->port;
	if (!host) {
		g_set_error_literal(err, CONFIG_ERROR, 2, "no recorder host configured ([recorder] host)");
		return NULL;
	}
	if (!cfg->username || !*cfg->username) {
		g_set_error_literal(err, CONFIG_ERROR, 2,
				    "no recorder username configured ([recorder] username, secrets.ini or "
				    "CCTV_OVERLAY_USERNAME)");
		return NULL;
	}
	if (!cfg->password || !*cfg->password) {
		g_set_error_literal(err, CONFIG_ERROR, 2,
				    "no recorder password configured (set CCTV_OVERLAY_PASSWORD or 'password' "
				    "in secrets.ini next to config.ini)");
		return NULL;
	}

	int sub = subtype_override >= 0 ? subtype_override : cam->subtype >= 0 ? cam->subtype : cfg->subtype;
	char *rest = config_expand_path(cam->path ? cam->path : cfg->path, cam->path == NULL, cam->channel, sub, err);
	if (!rest)
		return NULL;
	char *u = util_pct_encode(cfg->username);
	char *p = util_pct_encode(cfg->password);
	const char *lb = strchr(host, ':') ? "[" : "";
	const char *rb = strchr(host, ':') ? "]" : "";
	char *url = g_strdup_printf("rtsp://%s:%s@%s%s%s:%d%s", u, p, lb, host, rb, port, rest);
	util_wipe_free(u);
	util_wipe_free(p);
	g_free(rest);
	return url;
}
