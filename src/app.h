#ifndef CCTV_APP_H
#define CCTV_APP_H

#include "camera.h"
#include "config.h"
#include "state.h"

#include <json-glib/json-glib.h>

typedef struct App App;

typedef struct {
	gboolean overlay_enabled; /* FALSE for --no-overlay: overlay_* are never called */
	double backoff_scale;     /* multiplies the 0.5/1/2/4/8 s reconnect delays; tests use ~0.01 */
	void (*quit_cb)(void *user);
	void *quit_user;
} AppOptions;

#define APP_MAX_RETRIES 5

/* Takes ownership of cfg. Starts nothing: the app idles in HIDDEN. */
App *app_new(Config *cfg, const AppOptions *opt);
void app_free(App *a); /* hides (frees video/overlay) */

AppState app_state(const App *a);
const Config *app_config(const App *a);
const Camera *app_current_camera(const App *a);
guint app_current_index(const App *a);
guint app_retry_count(const App *a);
const char *app_last_error(const App *a); /* sanitised; NULL when none */

/* Commands (all return FALSE with *err set on failure). */
gboolean app_toggle(App *a, GError **err);
gboolean app_show(App *a, const char *camera_spec /* NULL = current */, GError **err);
gboolean app_next(App *a, GError **err);
gboolean app_prev(App *a, GError **err);
void app_hide(App *a);
gboolean app_reload(App *a, GError **err);
void app_request_quit(App *a);

/* IPC glue: matches IpcHandler / IpcAfterReply with user == App*. */
JsonObject *app_ipc_handler(void *user, const char *cmd, JsonObject *req);
void app_ipc_after_reply(void *user, const char *cmd);

#endif
