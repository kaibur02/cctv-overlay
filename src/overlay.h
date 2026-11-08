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
 *  - overlay_show(): map the window (monitor/anchor/size from cfg) with `ntiles` video tiles (1 = single
 *    camera, 2..4 = 2x2 grid), each labelled from `labels`. On a visible window with the same tile
 *    count only the labels change; a different count rebuilds the tiles, so the caller must stop all
 *    video slots first. cfg is only valid during the call; copy what is needed.
 *  - overlay_hide(): unmap, free the GL contexts. Idempotent. Stops every video slot first (render
 *    contexts must be freed while their GL context is still alive).
 *  - overlay_set_state(): update one tile's status dot / message. `detail` may be NULL; when
 *    non-NULL it is already redacted and safe to display.
 *  - overlay_set_audio(): show the small speaker icon while audio is enabled; `muted_view` draws it
 *    dimmed (grid view never plays audio).
 *
 * The GTK-specific video target accessor lives in overlay_gtk.h so this header (and app.c, the
 * stubs and the tests) stay free of GTK.
 */

#include "config.h"
#include "state.h"

gboolean overlay_init(GError **err);
void overlay_show(const Config *cfg, guint ntiles, const char *const *labels);
void overlay_hide(void);
void overlay_set_state(guint tile, AppState state, const char *detail);
void overlay_set_audio(gboolean enabled, gboolean muted_view);

#endif
