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
 *   CCTV_OVERLAY_STUB_FAIL_SLOT=N    restrict CCTV_OVERLAY_STUB_FAIL to slot N (grid tests)
 *   CCTV_OVERLAY_STUB_DROP_AFTER_MS=N  after the first frame, simulate a stall/error N ms later
 */

typedef struct {
	guint slot;
	guint session; /* active session id, 0 = none */
	guint src_frame, src_drop;
	gboolean audio;
	VideoCallbacks cb;
	void *user;
} Slot;

static Slot S[VIDEO_MAX_SLOTS];
static guint next_session, starts;

static long env_long(const char *name, long def)
{
	const char *v = g_getenv(name);
	return (v && *v) ? strtol(v, NULL, 10) : def;
}

static gboolean should_fail(guint slot)
{
	const char *v = g_getenv("CCTV_OVERLAY_STUB_FAIL");
	if (!v || !*v || strcmp(v, "0") == 0)
		return FALSE;
	if (env_long("CCTV_OVERLAY_STUB_FAIL_SLOT", -1) >= 0 && env_long("CCTV_OVERLAY_STUB_FAIL_SLOT", -1) != (long)slot)
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

static Slot *by_session(guint session)
{
	for (guint i = 0; i < VIDEO_MAX_SLOTS; i++)
		if (session && S[i].session == session)
			return &S[i];
	return NULL;
}

static gboolean drop_cb(gpointer data)
{
	Slot *s = by_session(GPOINTER_TO_UINT(data));
	if (!s)
		return G_SOURCE_REMOVE;
	s->src_drop = 0;
	s->cb.error(s->user, s->slot, s->session, "simulated stream stall");
	return G_SOURCE_REMOVE;
}

static gboolean frame_cb(gpointer data)
{
	Slot *s = by_session(GPOINTER_TO_UINT(data));
	if (!s)
		return G_SOURCE_REMOVE;
	s->src_frame = 0;
	if (should_fail(s->slot)) {
		s->cb.error(s->user, s->slot, s->session, "simulated stream failure");
		return G_SOURCE_REMOVE;
	}
	long drop = env_long("CCTV_OVERLAY_STUB_DROP_AFTER_MS", 0);
	if (drop > 0)
		s->src_drop = g_timeout_add((guint)drop, drop_cb, data);
	s->cb.first_frame(s->user, s->slot, s->session);
	return G_SOURCE_REMOVE;
}

guint video_start(guint slot, const Config *cfg, gboolean audio, VideoUrlBuilder build, void *build_user,
		  const VideoCallbacks *cb, void *cb_user, GError **err)
{
	(void)cfg;
	g_return_val_if_fail(slot < VIDEO_MAX_SLOTS, 0);
	video_stop(slot);

	char *url = build(build_user, slot, err); /* exercises the real credential path */
	if (!url)
		return 0;
	util_wipe_free(url);

	Slot *s = &S[slot];
	s->slot = slot;
	s->cb = *cb;
	s->user = cb_user;
	s->audio = audio;
	starts++;
	s->session = ++next_session;
	long delay = env_long("CCTV_OVERLAY_STUB_DELAY_MS", 150);
	s->src_frame = g_timeout_add((guint)(delay < 0 ? 0 : delay), frame_cb, GUINT_TO_POINTER(s->session));
	log_debug("video(stub): slot %u start session %u%s", slot, s->session, audio ? " (audio)" : "");
	return s->session;
}

void video_stop(guint slot)
{
	if (slot >= VIDEO_MAX_SLOTS)
		return;
	Slot *s = &S[slot];
	if (s->src_frame)
		g_source_remove(s->src_frame);
	if (s->src_drop)
		g_source_remove(s->src_drop);
	s->src_frame = s->src_drop = 0;
	if (s->session)
		log_debug("video(stub): slot %u stop session %u", slot, s->session);
	s->session = 0;
	s->audio = FALSE;
}

void video_stop_all(void)
{
	for (guint i = 0; i < VIDEO_MAX_SLOTS; i++)
		video_stop(i);
}

void video_set_audio(guint slot, gboolean on)
{
	if (slot < VIDEO_MAX_SLOTS && S[slot].session) {
		S[slot].audio = on;
		log_debug("video(stub): slot %u audio %s", slot, on ? "on" : "off");
	}
}

/* Test-only inspection (declared in the tests, not in video.h). */
gboolean video_stub_active(void)
{
	for (guint i = 0; i < VIDEO_MAX_SLOTS; i++)
		if (S[i].session)
			return TRUE;
	return FALSE;
}
guint video_stub_active_count(void)
{
	guint n = 0;
	for (guint i = 0; i < VIDEO_MAX_SLOTS; i++)
		n += S[i].session != 0;
	return n;
}
gboolean video_stub_audio(guint slot) { return slot < VIDEO_MAX_SLOTS && S[slot].audio; }
guint video_stub_start_count(void) { return starts; }
