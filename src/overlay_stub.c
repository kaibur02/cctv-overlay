#define _GNU_SOURCE
#include "overlay.h"

#include "logging.h"

#include <stdbool.h>

static gboolean g_visible;
static char *g_label;
static AppState g_state = APP_HIDDEN;

gboolean overlay_init(GError **err)
{
	(void)err;
	return TRUE;
}

void overlay_show(const Config *cfg, const char *label)
{
	(void)cfg;
	g_visible = TRUE;
	g_free(g_label);
	g_label = g_strdup(label);
	log_debug("overlay(stub): show '%s'", label);
}

void overlay_hide(void)
{
	if (g_visible)
		log_debug("overlay(stub): hide");
	g_visible = FALSE;
	g_state = APP_HIDDEN;
}

void overlay_set_state(AppState state, const char *detail)
{
	g_state = state;
	log_debug("overlay(stub): state=%s%s%s", app_state_name(state), detail ? " detail=" : "",
		  detail ? detail : "");
}

void overlay_set_label(const char *label)
{
	g_free(g_label);
	g_label = g_strdup(label);
	log_debug("overlay(stub): label '%s'", label);
}

/* Test-only inspection (declared in the tests, not in overlay.h). */
gboolean overlay_stub_visible(void) { return g_visible; }
void overlay_stub_simulate_lost(void) { g_visible = FALSE; } /* window destroyed behind the app's back */
const char *overlay_stub_label(void) { return g_label; }
AppState overlay_stub_state(void) { return g_state; }
