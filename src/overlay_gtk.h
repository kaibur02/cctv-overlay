#ifndef CCTV_OVERLAY_GTK_H
#define CCTV_OVERLAY_GTK_H

/* GTK-only part of the overlay interface, used by video_mpv.c. Not included by the core. */

#include <gtk/gtk.h>

/* The GtkGLArea of tile `slot` in the visible overlay window, or NULL (hidden / no such tile). */
GtkGLArea *overlay_gtk_gl_area(guint slot);

/* Render hook for a tile's GL area "render" signal. With no hook installed the area is cleared to
 * black. Same contract as the signal: return TRUE when the frame was handled. */
typedef gboolean (*OverlayRenderFn)(GtkGLArea *area, void *user);
void overlay_gtk_set_render_fn(guint slot, OverlayRenderFn fn, void *user);

/* The engine reports whether the stream in `slot` carries an audio track. In the single-camera view the
 * speaker icon is crossed out when audio is enabled but the stream has none. */
void overlay_gtk_set_audio_track(guint slot, gboolean present);

#endif
