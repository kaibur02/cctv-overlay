#define _GNU_SOURCE
#include "overlay.h"

#include "logging.h"

static gboolean g_visible, g_audio;
static char *g_label; /* single view: the camera name; grid: names joined with '|' */
static guint g_ntiles;
static AppState g_state = APP_HIDDEN;

gboolean overlay_init(GError **err)
{
	(void)err;
	return TRUE;
}

void overlay_show(const Config *cfg, guint ntiles, const char *const *labels)
{
	(void)cfg;
	g_visible = TRUE;
	g_ntiles = ntiles;
	g_free(g_label);
	GString *j = g_string_new(NULL);
	for (guint i = 0; i < ntiles; i++)
		g_string_append_printf(j, "%s%s", i ? "|" : "", labels[i]);
	g_label = g_string_free(j, FALSE);
	log_debug("overlay(stub): show %u tile(s) '%s'", ntiles, g_label);
}

void overlay_hide(void)
{
	if (g_visible)
		log_debug("overlay(stub): hide");
	g_visible = FALSE;
	g_state = APP_HIDDEN;
}

void overlay_set_state(guint tile, AppState state, const char *detail)
{
	if (tile == 0)
		g_state = state;
	log_debug("overlay(stub): tile %u state=%s%s%s", tile, app_state_name(state), detail ? " detail=" : "",
		  detail ? detail : "");
}

void overlay_set_audio(gboolean enabled, gboolean muted_view)
{
	g_audio = enabled;
	log_debug("overlay(stub): audio icon %s%s", enabled ? "on" : "off", enabled && muted_view ? " (dimmed)" : "");
}

/* Test-only inspection (declared in the tests, not in overlay.h). */
gboolean overlay_stub_visible(void) { return g_visible; }
void overlay_stub_simulate_lost(void) { g_visible = FALSE; } /* window destroyed behind the app's back */
const char *overlay_stub_label(void) { return g_label; }
guint overlay_stub_tiles(void) { return g_ntiles; }
gboolean overlay_stub_audio(void) { return g_audio; }
AppState overlay_stub_state(void) { return g_state; }
