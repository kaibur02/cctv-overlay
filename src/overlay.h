#ifndef CCTV_OVERLAY_H
#define CCTV_OVERLAY_H

/* Overlay window interface.
 *
 * Implementations: overlay_stub.c (M2: only logs) and, later, the GTK4 + layer-shell window.
 * All calls happen on the main-loop thread and must be cheap and non-blocking.
 *
 *  - overlay_show(): map the window (monitor/anchor/size from cfg), showing `label`.
 *    cfg is only valid during the call; copy what is needed.
 *  - overlay_hide(): unmap and drop textures. Idempotent.
 *  - overlay_set_state(): update the status dot / message. `detail` may be NULL; when
 *    non-NULL it is already redacted and safe to display.
 *  - overlay_set_label(): change the camera label while visible (camera switch).
 *  - The video widget attaches to the window in M3/M4; the interface will grow a
 *    "get GL area" accessor then. Keeping it out for now keeps M2 headless.
 */

#include "config.h"
#include "state.h"

void overlay_show(const Config *cfg, const char *label);
void overlay_hide(void);
void overlay_set_state(AppState state, const char *detail);
void overlay_set_label(const char *label);

#endif
