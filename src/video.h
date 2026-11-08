#ifndef CCTV_VIDEO_H
#define CCTV_VIDEO_H

/* Video engine interface.
 *
 * Implementations: video_stub.c (simulated; `make check`, `make STUB=1`) and video_mpv.c (libmpv
 * render API drawing into the overlay's GtkGLArea; the default build). The Makefile picks one.
 *
 * Contract (the real engine must honour it):
 *  - Everything is called on, and every callback is delivered on, the GLib main-loop thread.
 *    Engine threads marshal events with g_idle_add(); callbacks are never invoked from inside
 *    video_start() or video_stop().
 *  - At most one session exists. video_start() on a running session stops it first.
 *  - video_start() invokes `build` synchronously, exactly once, before returning. The returned
 *    URL contains credentials: the engine uses it for the load call and then releases it with
 *    util_wipe_free(). It must never be logged, stored, or put into an error message.
 *    If `build` fails, video_start() returns 0 with *err set (no retry makes sense).
 *  - The engine copies whatever it needs out of `cfg` (transport, hwdec, ...) during
 *    video_start(); `cfg` may be freed after it returns.
 *  - Callbacks carry the session id returned by video_start(). After video_stop() no callback
 *    for that session may fire; the app also ignores stale ids.
 *  - Error strings are for display/status. They must not contain URLs or credentials (the app
 *    additionally passes them through log_redact()).
 *  - Stall detection (no frame for ~10 s while live) and mpv end-file errors are the engine's
 *    job: report them through `error`. Retry/backoff policy lives in app.c, not in the engine.
 *  - video_stop() is idempotent and frees the player, network connection and GL state.
 *  - The mpv engine needs the overlay window (overlay_show() first) and fails video_start() with a
 *    message when there is none (e.g. `daemon --no-overlay` on the real build).
 */

#include "config.h"

#include <glib.h>

typedef char *(*VideoUrlBuilder)(void *user, GError **err);

typedef struct {
	void (*first_frame)(void *user, guint session);
	void (*error)(void *user, guint session, const char *message);
	void (*ended)(void *user, guint session); /* clean end of stream (treated as an error by app.c) */
} VideoCallbacks;

/* Returns a non-zero session id, or 0 on failure with *err set. */
guint video_start(const Config *cfg, VideoUrlBuilder build, void *build_user, const VideoCallbacks *cb,
		  void *cb_user, GError **err);
void video_stop(void);

#endif
