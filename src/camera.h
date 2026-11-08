#ifndef CCTV_CAMERA_H
#define CCTV_CAMERA_H

#include "config.h"

/* Ordered view over cfg->cameras plus the "current" cursor. Does not own the Config:
 * the Config must outlive the list (app.c replaces both together on reload). */
typedef struct CameraList CameraList;

CameraList *camera_list_new(const Config *cfg);
void camera_list_free(CameraList *l);

guint camera_list_count(const CameraList *l);
const Camera *camera_list_get(const CameraList *l, guint index);
guint camera_list_current(const CameraList *l);
const Camera *camera_list_current_camera(const CameraList *l);
void camera_list_set_current(CameraList *l, guint index);

/* Advance the cursor with wraparound and return the new index. */
guint camera_list_next(CameraList *l);
guint camera_list_prev(CameraList *l);

/* spec: exact camera name (case-insensitive), then id, then a zero-based decimal index. */
gboolean camera_list_find(const CameraList *l, const char *spec, guint *index);
gboolean camera_list_find_id(const CameraList *l, const char *id, guint *index);

/* Build the stream URL into a new heap string. The caller MUST release it with util_wipe_free().
 * Fails (without leaking details) when username/password are missing. The URL, or any part of
 * it, is never stored or put into an error message. */
char *camera_resolve_url(const Config *cfg, const Camera *cam, int subtype_override /* -1 = none */,
			 GError **err);

#endif
