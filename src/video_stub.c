#define _GNU_SOURCE
#include "video.h"

#include "logging.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

/* Simulated engine. Knobs (environment, read at every start):
 *   CCTV_OVERLAY_STUB_DELAY_MS=N     first-frame delay, default 150
 *   CCTV_OVERLAY_STUB_FAIL=1         every start fails after the delay ("always" also accepted)
 *   CCTV_OVERLAY_STUB_FAIL=first:N   only the first N starts after the value was set fail
 *   CCTV_OVERLAY_STUB_DROP_AFTER_MS=N  after the first frame, simulate a stall/error N ms later
 */

static struct {
	guint next_session;
	guint session; /* active session id, 0 = none */
	guint src_frame, src_drop;
	guint starts;
	VideoCallbacks cb;
	void *user;
} S;

static long env_long(const char *name, long def)
{
	const char *v = g_getenv(name);
	return (v && *v) ? strtol(v, NULL, 10) : def;
}

static gboolean should_fail(void)
{
	const char *v = g_getenv("CCTV_OVERLAY_STUB_FAIL");
	if (!v || !*v || strcmp(v, "0") == 0)
		return FALSE;
	if (g_str_has_prefix(v, "first:")) {
		static char *last_spec;
		static guint failed;
		if (g_strcmp0(last_spec, v) != 0) { /* new setting: count from zero again */
			g_free(last_spec);
			last_spec = g_strdup(v);
			failed = 0;
		}
		return failed++ < (guint)strtol(v + 6, NULL, 10);
	}
	return TRUE;
}

static gboolean drop_cb(gpointer data)
{
	guint session = GPOINTER_TO_UINT(data);
	S.src_drop = 0;
	if (session == S.session)
		S.cb.error(S.user, session, "simulated stream stall");
	return G_SOURCE_REMOVE;
}

static gboolean frame_cb(gpointer data)
{
	guint session = GPOINTER_TO_UINT(data);
	S.src_frame = 0;
	if (session != S.session)
		return G_SOURCE_REMOVE;
	if (should_fail()) {
		S.cb.error(S.user, session, "simulated stream failure");
		return G_SOURCE_REMOVE;
	}
	long drop = env_long("CCTV_OVERLAY_STUB_DROP_AFTER_MS", 0);
	if (drop > 0)
		S.src_drop = g_timeout_add((guint)drop, drop_cb, data);
	S.cb.first_frame(S.user, session);
	return G_SOURCE_REMOVE;
}

guint video_start(const Config *cfg, VideoUrlBuilder build, void *build_user, const VideoCallbacks *cb,
		  void *cb_user, GError **err)
{
	(void)cfg;
	video_stop();

	char *url = build(build_user, err); /* exercises the real credential path */
	if (!url)
		return 0;
	util_wipe_free(url);

	S.cb = *cb;
	S.user = cb_user;
	S.starts++;
	S.session = ++S.next_session;
	long delay = env_long("CCTV_OVERLAY_STUB_DELAY_MS", 150);
	S.src_frame = g_timeout_add((guint)(delay < 0 ? 0 : delay), frame_cb, GUINT_TO_POINTER(S.session));
	log_debug("video(stub): start session %u", S.session);
	return S.session;
}

void video_stop(void)
{
	if (S.src_frame)
		g_source_remove(S.src_frame);
	if (S.src_drop)
		g_source_remove(S.src_drop);
	S.src_frame = S.src_drop = 0;
	if (S.session)
		log_debug("video(stub): stop session %u", S.session);
	S.session = 0;
}

/* Test-only inspection (declared in the tests, not in video.h). */
gboolean video_stub_active(void) { return S.session != 0; }
guint video_stub_start_count(void) { return S.starts; }
