#ifndef CCTV_OVERLAY_GTK_H
#define CCTV_OVERLAY_GTK_H

/* GTK-only part of the overlay interface, used by video_mpv.c. Not included by the core. */

#include <gtk/gtk.h>

/* The GtkGLArea inside the visible overlay window, or NULL while hidden. */
GtkGLArea *overlay_gtk_gl_area(void);

/* Render hook for the GL area's "render" signal. With no hook installed the area is cleared to
 * black. Same contract as the signal: return TRUE when the frame was handled. */
typedef gboolean (*OverlayRenderFn)(GtkGLArea *area, void *user);
void overlay_gtk_set_render_fn(OverlayRenderFn fn, void *user);

#endif
