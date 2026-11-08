#ifndef CCTV_OVERLAY_H
#define CCTV_OVERLAY_H

/* Overlay window interface.
 *
 * Implementations: overlay_stub.c (only logs; used by `make check`) and overlay_gtk.c (GTK4 +
 * gtk4-layer-shell window with a GtkGLArea for the libmpv engine).
 * All calls happen on the main-loop thread and must be cheap and non-blocking.
 *
 *  - overlay_init(): called once at daemon start, before any other call, when the overlay is
 *    enabled. The GTK implementation opens the Wayland display and verifies that layer-shell
 *    works; on failure it returns FALSE with *err set to a message fit for the terminal.
 *  - overlay_show(): map the window (monitor/anchor/size from cfg), showing `label`.
 *    cfg is only valid during the call; copy what is needed.
 *  - overlay_hide(): unmap, free the GL context. Idempotent. Stops the video session first (the
 *    render context must be freed while the GL context is still alive).
 *  - overlay_set_state(): update the status dot / message. `detail` may be NULL; when
 *    non-NULL it is already redacted and safe to display.
 *  - overlay_set_label(): change the camera label while visible (camera switch).
 *
 * The GTK-specific video target accessor lives in overlay_gtk.h so this header (and app.c, the
 * stubs and the tests) stay free of GTK.
 */

#include "config.h"
#include "state.h"

gboolean overlay_init(GError **err);
void overlay_show(const Config *cfg, const char *label);
void overlay_hide(void);
void overlay_set_state(AppState state, const char *detail);
void overlay_set_label(const char *label);

#endif
