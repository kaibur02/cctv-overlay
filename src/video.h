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
 *  - Up to VIDEO_MAX_SLOTS independent players ("slots", one per overlay tile). Each slot has at most
 *    one session; video_start() on a busy slot stops it first. Session ids are unique across slots.
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
 *  - The mpv engine needs the overlay tile for the slot (overlay_show() first) and fails video_start()
 *    with a message when there is none (e.g. `daemon --no-overlay` on the real build).
 *  - Audio: `audio` selects the stream's audio track at start; video_set_audio() switches it live.
 *    Audio is never selected otherwise (no audio output is opened).
 */

#include "config.h"

#include <glib.h>

#define VIDEO_MAX_SLOTS 4

typedef char *(*VideoUrlBuilder)(void *user, guint slot, GError **err);

typedef struct {
	void (*first_frame)(void *user, guint slot, guint session);
	void (*error)(void *user, guint slot, guint session, const char *message);
	void (*ended)(void *user, guint slot, guint session); /* clean end (treated as an error by app.c) */
} VideoCallbacks;

/* Returns a non-zero session id, or 0 on failure with *err set. */
guint video_start(guint slot, const Config *cfg, gboolean audio, VideoUrlBuilder build, void *build_user,
		  const VideoCallbacks *cb, void *cb_user, GError **err);
void video_stop(guint slot);
void video_stop_all(void);
void video_set_audio(guint slot, gboolean on);

#endif
