/*
 * Disposable probe: GTK4 + GtkGLArea + libmpv render API.
 *
 * Not product code. Reads a single playback source (plain path or URL) from a
 * file, plays it with low-latency options, renders into the GLArea FBO and
 * reports timing, decoder and video parameters. See spike/README.md.
 *
 * Threading model:
 *   - GTK main thread: owns the GL context, all mpv_render_* calls, and all
 *     mpv control commands (sent async so it never blocks).
 *   - "mpv-events" thread: mpv_wait_event() only. The render update callback
 *     (arbitrary thread, no mpv calls allowed) just sets a flag and posts an
 *     idle source; the main thread then calls mpv_render_context_update().
 */

#define _POSIX_C_SOURCE 200809L

#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <gdk/gdk.h>
#include <glib-unix.h>
#include <gtk/gtk.h>
#include <gtk4-layer-shell/gtk4-layer-shell.h>
#include <mpv/client.h>
#include <mpv/render_gl.h>

#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/wayland/gdkwayland.h>
#endif

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define APP_ID "com.example.cctv.mpvgl.spike"

/*
 * mpv's EGL entry points are enough for the Wayland/EGL path this probe
 * targets. (An X11/GLX build would need gdk_gl_context_get_proc_address.)
 */
static void *egl_get_proc_address(void *ctx, const char *name)
{
    (void)ctx;
    return epoxy_eglGetProcAddress(name);
}

struct app_state {
    mpv_handle *mpv;
    /* Only written on the GTK main thread; read atomically by mpv threads. */
    mpv_render_context *render_ctx;

    GtkApplication *app;
    GtkWindow *window;
    GtkGLArea *gl_area;
    GThread *event_thread;

    char *url;
    double duration_sec;
    int benchmark;
    char *hwdec;
    char *rtsp_transport;
    char *layer_monitor;   /* connector name -> layer-shell overlay mode */
    int width;
    int height;

    struct timespec t0_run;
    struct timespec t0_loadfile;
    struct timespec t0_first_frame;
    int have_t0_loadfile;
    int first_frame_reported;

    gint stop_requested;  /* atomic */
    gint frame_pending;   /* atomic: update callback fired since last render */
    gint setup_failed;    /* atomic */
    gint playback_failed; /* atomic */
    gint frames_rendered; /* main thread only */

    /* Diagnostics filled in by the event thread, read after g_thread_join(). */
    char *video_codec;
    char *video_pixfmt;
    int video_w;
    int video_h;
    double container_fps;
    char *hwdec_current;
};

static double timespec_diff_ms(const struct timespec *a, const struct timespec *b)
{
    return (b->tv_sec - a->tv_sec) * 1000.0 + (b->tv_nsec - a->tv_nsec) / 1e6;
}

static void now(struct timespec *ts)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
}

static mpv_render_context *render_ctx_get(struct app_state *st)
{
    return g_atomic_pointer_get(&st->render_ctx);
}

static void render_ctx_set(struct app_state *st, mpv_render_context *ctx)
{
    g_atomic_pointer_set(&st->render_ctx, ctx);
}

static void request_stop(struct app_state *st);
static gboolean request_stop_idle(gpointer userdata);

/* ------------------------------------------------------------------ */
/* mpv render update -> GTK main loop                                 */
/* ------------------------------------------------------------------ */

/*
 * Runs on the GTK main thread (the render thread). mpv_render_context_update()
 * must be called here, not from the update callback itself.
 */
static gboolean render_update_idle(gpointer userdata)
{
    struct app_state *st = userdata;

    mpv_render_context *ctx = render_ctx_get(st);
    if (!ctx || g_atomic_int_get(&st->stop_requested))
        return G_SOURCE_REMOVE;

    uint64_t flags = mpv_render_context_update(ctx);
    if (flags & MPV_RENDER_UPDATE_FRAME) {
        /* Set only when mpv really has a video frame to render. */
        g_atomic_int_set(&st->frame_pending, 1);
        if (st->gl_area)
            gtk_gl_area_queue_render(st->gl_area);
    }

    return G_SOURCE_REMOVE;
}

/* Called from an arbitrary mpv thread: no mpv API, no GTK API. */
static void on_mpv_render_update(void *userdata)
{
    struct app_state *st = userdata;
    if (!st || g_atomic_int_get(&st->stop_requested))
        return;

    g_idle_add(render_update_idle, st); /* thread-safe */
}

/* ------------------------------------------------------------------ */
/* GTK GLArea render callback                                         */
/* ------------------------------------------------------------------ */

static gboolean on_gl_render(GtkGLArea *area, GdkGLContext *context, gpointer userdata)
{
    struct app_state *st = userdata;
    (void)context;

    mpv_render_context *ctx = render_ctx_get(st);
    if (!ctx)
        return TRUE;

    int w = gtk_widget_get_width(GTK_WIDGET(area));
    int h = gtk_widget_get_height(GTK_WIDGET(area));
    if (w <= 0 || h <= 0)
        return TRUE;

    int fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);

    mpv_opengl_fbo mpv_fbo = {
        .fbo = fbo,
        .w = w,
        .h = h,
        .internal_format = 0,
    };

    int flip_y = 1;

    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &mpv_fbo},
        {MPV_RENDER_PARAM_FLIP_Y, &flip_y},
        {MPV_RENDER_PARAM_INVALID, NULL},
    };

    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fbo);
    glViewport(0, 0, w, h);

    mpv_render_context_render(ctx, params);

    /* Did mpv signal a new frame since the previous render? */
    if (g_atomic_int_compare_and_exchange(&st->frame_pending, 1, 0)) {
        st->frames_rendered++;
        if (!st->first_frame_reported) {
            struct timespec t;
            now(&t);
            st->first_frame_reported = 1;
            st->t0_first_frame = t;
            if (st->have_t0_loadfile) {
                printf("first-frame latency (loadfile -> first rendered frame): %.3f ms\n",
                       timespec_diff_ms(&st->t0_loadfile, &t));
            } else {
                printf("first-frame latency: first frame rendered (loadfile time unknown)\n");
            }
        }
    }

    return TRUE;
}

static void on_gl_realize(GtkGLArea *area, gpointer userdata)
{
    struct app_state *st = userdata;

    printf("gl area realize\n");

    gtk_gl_area_make_current(area);
    GError *gl_err = gtk_gl_area_get_error(area);
    if (gl_err != NULL) {
        fprintf(stderr, "GL area failed to create context: %s\n", gl_err->message);
        g_atomic_int_set(&st->setup_failed, 1);
        g_idle_add(request_stop_idle, st);
        return;
    }

    if (render_ctx_get(st))
        return; /* already created (re-realize) */

    mpv_opengl_init_params gl_init_params = {
        .get_proc_address = egl_get_proc_address,
        .get_proc_address_ctx = NULL,
    };

    struct wl_display *wl = NULL;
#ifdef GDK_WINDOWING_WAYLAND
    GdkDisplay *gdk_display = gdk_display_get_default();
    if (gdk_display && GDK_IS_WAYLAND_DISPLAY(gdk_display))
        wl = gdk_wayland_display_get_wl_display(gdk_display);
#endif

    mpv_render_param params[4];
    int n = 0;
    params[n].type = MPV_RENDER_PARAM_API_TYPE;
    params[n++].data = (void *)MPV_RENDER_API_TYPE_OPENGL;
    params[n].type = MPV_RENDER_PARAM_OPENGL_INIT_PARAMS;
    params[n++].data = &gl_init_params;
    if (wl) {
        params[n].type = MPV_RENDER_PARAM_WL_DISPLAY;
        params[n++].data = wl;
        printf("render context: including WL_DISPLAY\n");
    } else {
        printf("render context: no WL_DISPLAY (not Wayland or GDK_WINDOWING_WAYLAND unavailable)\n");
    }
    params[n].type = MPV_RENDER_PARAM_INVALID;
    params[n].data = NULL;

    mpv_render_context *ctx = NULL;
    int err = mpv_render_context_create(&ctx, st->mpv, params);
    if (err < 0) {
        fprintf(stderr, "render context create failed: %s\n", mpv_error_string(err));
        g_atomic_int_set(&st->setup_failed, 1);
        g_idle_add(request_stop_idle, st);
        return;
    }

    /* Publish before installing the callback: it fires immediately. */
    render_ctx_set(st, ctx);
    mpv_render_context_set_update_callback(ctx, on_mpv_render_update, st);
    printf("render context: created\n");

    /* Safe to load now: the render context exists before a VO is created. */
    const char *cmd[] = {"loadfile", st->url, NULL};
    now(&st->t0_loadfile);
    st->have_t0_loadfile = 1;
    err = mpv_command_async(st->mpv, 0, cmd);
    if (err < 0)
        fprintf(stderr, "loadfile failed: %s\n", mpv_error_string(err));
}

static void on_gl_unrealize(GtkGLArea *area, gpointer userdata)
{
    struct app_state *st = userdata;

    mpv_render_context *ctx = render_ctx_get(st);
    if (!ctx)
        return;

    /* The GL context must be current when the render API is torn down. */
    gtk_gl_area_make_current(area);
    if (gtk_gl_area_get_error(area) == NULL) {
        render_ctx_set(st, NULL);
        mpv_render_context_free(ctx);
        printf("render context: freed (unrealize)\n");
    } else {
        render_ctx_set(st, NULL);
        fprintf(stderr, "warning: GL context invalid at unrealize; render context leaked\n");
    }
}

/* ------------------------------------------------------------------ */
/* mpv event loop (runs in its own thread)                            */
/* ------------------------------------------------------------------ */

static gpointer mpv_event_thread(gpointer userdata)
{
    struct app_state *st = userdata;

    while (!g_atomic_int_get(&st->stop_requested)) {
        mpv_event *event = mpv_wait_event(st->mpv, -1);
        if (event->event_id == MPV_EVENT_NONE)
            continue;

        if (event->event_id == MPV_EVENT_SHUTDOWN)
            break;

        if (event->event_id == MPV_EVENT_LOG_MESSAGE) {
            mpv_event_log_message *msg = event->data;
            if (msg->log_level <= MPV_LOG_LEVEL_WARN)
                printf("mpv [%s] %s", msg->prefix, msg->text);
            continue;
        }

        if (event->event_id == MPV_EVENT_FILE_LOADED) {
            if (st->have_t0_loadfile) {
                struct timespec t;
                now(&t);
                printf("file-loaded latency (loadfile -> demuxer ready): %.3f ms\n",
                       timespec_diff_ms(&st->t0_loadfile, &t));
            }
            continue;
        }

        if (event->event_id == MPV_EVENT_END_FILE) {
            mpv_event_end_file *ef = event->data;
            printf("end-file reason=%d\n", ef->reason);
            if (ef->reason == MPV_END_FILE_REASON_ERROR) {
                printf("playback error: %s\n", mpv_error_string(ef->error));
                g_atomic_int_set(&st->playback_failed, 1);
            }
            if (ef->reason == MPV_END_FILE_REASON_EOF)
                g_idle_add(request_stop_idle, st);
            continue;
        }

        if (event->event_id == MPV_EVENT_PROPERTY_CHANGE) {
            mpv_event_property *prop = event->data;
            if (!prop || !prop->name)
                continue;

            if (strcmp(prop->name, "video-params") == 0 && prop->data &&
                prop->format == MPV_FORMAT_NODE) {
                mpv_node *node = prop->data;
                if (node->format == MPV_FORMAT_NODE_MAP && node->u.list) {
                    for (int i = 0; i < node->u.list->num; i++) {
                        const char *k = node->u.list->keys[i];
                        mpv_node *v = &node->u.list->values[i];
                        if (strcmp(k, "w") == 0 && v->format == MPV_FORMAT_INT64)
                            st->video_w = (int)v->u.int64;
                        if (strcmp(k, "h") == 0 && v->format == MPV_FORMAT_INT64)
                            st->video_h = (int)v->u.int64;
                        if (strcmp(k, "pixelformat") == 0 && v->format == MPV_FORMAT_STRING) {
                            free(st->video_pixfmt);
                            st->video_pixfmt = v->u.string ? strdup(v->u.string) : NULL;
                        }
                    }
                    printf("video-params: %dx%d %.3f fps pixfmt=%s\n",
                           st->video_w, st->video_h, st->container_fps,
                           st->video_pixfmt ? st->video_pixfmt : "(unknown)");
                }
            }
            if (strcmp(prop->name, "container-fps") == 0 && prop->data &&
                prop->format == MPV_FORMAT_DOUBLE) {
                st->container_fps = *(double *)prop->data;
                printf("container-fps: %.3f\n", st->container_fps);
            }
            if (strcmp(prop->name, "video-codec") == 0 && prop->data &&
                prop->format == MPV_FORMAT_STRING) {
                char *val = *(char **)prop->data;
                free(st->video_codec);
                st->video_codec = val ? strdup(val) : NULL;
                if (st->video_codec)
                    printf("video-codec: %s\n", st->video_codec);
            }
            if (strcmp(prop->name, "hwdec-current") == 0 && prop->data &&
                prop->format == MPV_FORMAT_STRING) {
                char *val = *(char **)prop->data;
                free(st->hwdec_current);
                st->hwdec_current = val ? strdup(val) : NULL;
                if (st->hwdec_current)
                    printf("hwdec-current: %s\n", st->hwdec_current);
            }
            continue;
        }

        printf("mpv event: %s\n", mpv_event_name(event->event_id));
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/* App lifecycle                                                      */
/* ------------------------------------------------------------------ */

static gboolean on_close_request(GtkWindow *win, gpointer userdata)
{
    (void)win;
    request_stop((struct app_state *)userdata);
    return TRUE; /* request_stop() destroys the window itself */
}

static void on_layer_map(GtkWidget *w, gpointer userdata)
{
    (void)userdata;
    GdkSurface *surf = gtk_native_get_surface(GTK_NATIVE(w));
    if (surf) {
        cairo_region_t *empty = cairo_region_create();
        gdk_surface_set_input_region(surf, empty);
        cairo_region_destroy(empty);
        printf("layer-shell: empty input region applied\n");
    }
}

static void on_activate(GtkApplication *app, gpointer userdata)
{
    struct app_state *st = userdata;

    printf("activate\n");

    GtkWindow *win = GTK_WINDOW(gtk_application_window_new(app));
    st->window = win;
    gtk_window_set_title(win, "mpv GL spike");
    gtk_window_set_default_size(win, st->width, st->height);
    g_signal_connect(win, "close-request", G_CALLBACK(on_close_request), st);

    if (st->layer_monitor) {
        /* Layer-shell overlay mode: top-right, overlay layer, no keyboard,
         * empty input region (click-through). gtk4-layer-shell must be loaded
         * before libwayland (link order in the Makefile). */
        if (!gtk_layer_is_supported()) {
            fprintf(stderr, "layer-shell is NOT supported/active (library load order?)\n");
        } else {
            gtk_layer_init_for_window(win);
            gtk_layer_set_namespace(win, "cctv-overlay-spike-mpv");
            gtk_layer_set_layer(win, GTK_LAYER_SHELL_LAYER_OVERLAY);
            gtk_layer_set_keyboard_mode(win, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
            gtk_layer_set_exclusive_zone(win, -1);
            gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_TOP, TRUE);
            gtk_layer_set_anchor(win, GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
            gtk_layer_set_margin(win, GTK_LAYER_SHELL_EDGE_TOP, 24);
            gtk_layer_set_margin(win, GTK_LAYER_SHELL_EDGE_RIGHT, 24);
            GListModel *mons = gdk_display_get_monitors(gdk_display_get_default());
            for (guint i = 0; i < g_list_model_get_n_items(mons); i++) {
                GdkMonitor *m = g_list_model_get_item(mons, i);
                const char *conn = gdk_monitor_get_connector(m);
                if (conn && strcmp(conn, st->layer_monitor) == 0)
                    gtk_layer_set_monitor(win, m);
                g_object_unref(m);
            }
            gtk_window_set_decorated(win, FALSE);
            gtk_widget_set_focusable(GTK_WIDGET(win), FALSE);
            g_signal_connect(win, "map", G_CALLBACK(on_layer_map), st);
            printf("layer-shell: overlay on %s\n", st->layer_monitor);
        }
    }

    GtkWidget *gl = gtk_gl_area_new();
    gtk_gl_area_set_has_depth_buffer(GTK_GL_AREA(gl), FALSE);
    gtk_gl_area_set_has_stencil_buffer(GTK_GL_AREA(gl), FALSE);
    gtk_gl_area_set_allowed_apis(GTK_GL_AREA(gl), GDK_GL_API_GL | GDK_GL_API_GLES);
    gtk_gl_area_set_required_version(GTK_GL_AREA(gl), 0, 0);
    g_signal_connect(gl, "realize", G_CALLBACK(on_gl_realize), st);
    g_signal_connect(gl, "unrealize", G_CALLBACK(on_gl_unrealize), st);
    g_signal_connect(gl, "render", G_CALLBACK(on_gl_render), st);

    st->gl_area = GTK_GL_AREA(gl);
    gtk_window_set_child(win, gl);

    gtk_window_present(win);
}

/*
 * Single teardown path. Runs on the GTK main thread only. Frees the render
 * context with the GL context current before the window (and its GL context)
 * goes away, then asks mpv to quit and stops the GTK loop.
 */
static void request_stop(struct app_state *st)
{
    if (!g_atomic_int_compare_and_exchange(&st->stop_requested, 0, 1))
        return;

    mpv_render_context *ctx = render_ctx_get(st);
    if (ctx) {
        int freed = 0;
        if (st->gl_area && gtk_widget_get_realized(GTK_WIDGET(st->gl_area)) &&
            gtk_gl_area_get_error(st->gl_area) == NULL) {
            gtk_gl_area_make_current(st->gl_area);
            if (gtk_gl_area_get_error(st->gl_area) == NULL) {
                render_ctx_set(st, NULL);
                mpv_render_context_free(ctx);
                freed = 1;
                printf("render context: freed\n");
            }
        }
        if (!freed) {
            fprintf(stderr, "warning: freeing render context without a current GL context\n");
            render_ctx_set(st, NULL);
            mpv_render_context_free(ctx);
        }
    }

    st->gl_area = NULL;

    if (st->window) {
        gtk_window_destroy(st->window);
        st->window = NULL;
    }

    /* Async: safe from the render thread and never blocks on the core. */
    if (st->mpv) {
        const char *cmd[] = {"quit", NULL};
        mpv_command_async(st->mpv, 0, cmd);
    }

    if (st->app)
        g_application_quit(G_APPLICATION(st->app));
}

static gboolean request_stop_idle(gpointer userdata)
{
    request_stop((struct app_state *)userdata);
    return G_SOURCE_REMOVE;
}

static gboolean on_duration_timeout(gpointer userdata)
{
    request_stop((struct app_state *)userdata);
    return G_SOURCE_REMOVE;
}

static gboolean on_sigint(gpointer userdata)
{
    printf("SIGINT: stopping\n");
    request_stop((struct app_state *)userdata);
    return G_SOURCE_REMOVE;
}

static int set_opt(mpv_handle *mpv, const char *name, const char *value)
{
    int err = mpv_set_option_string(mpv, name, value);
    if (err < 0)
        fprintf(stderr, "option '%s=%s' failed: %s\n", name, value, mpv_error_string(err));
    return err;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s --url-file PATH [--duration SEC] [--benchmark] "
            "[--hwdec vaapi|vaapi-copy|auto-safe|no] [--rtsp-transport tcp|udp] "
            "[--size WxH]\n"
            "\n"
            "  --url-file PATH   file containing one playback source (plain path or URL)\n"
            "  --duration SEC    wall-clock seconds before the probe quits (0 = until EOF/SIGINT)\n"
            "  --benchmark       untimed playback (render as fast as the frame clock allows)\n"
            "  --hwdec MODE      mpv hwdec mode (default vaapi)\n"
            "  --rtsp-transport  rtsp-transport for RTSP sources (default tcp)\n"
            "  --size WxH        window size (default 960x540)\n",
            prog);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    struct app_state st;
    memset(&st, 0, sizeof(st));
    st.duration_sec = 0;
    st.width = 960;
    st.height = 540;
    st.hwdec = strdup("vaapi");
    st.rtsp_transport = strdup("tcp");

    const char *url_file = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--url-file") == 0 && i + 1 < argc) {
            url_file = argv[++i];
        } else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            st.duration_sec = atof(argv[++i]);
        } else if (strcmp(argv[i], "--benchmark") == 0) {
            st.benchmark = 1;
        } else if (strcmp(argv[i], "--hwdec") == 0 && i + 1 < argc) {
            free(st.hwdec);
            st.hwdec = strdup(argv[++i]);
        } else if (strcmp(argv[i], "--rtsp-transport") == 0 && i + 1 < argc) {
            free(st.rtsp_transport);
            st.rtsp_transport = strdup(argv[++i]);
        } else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            int w = 0, h = 0;
            if (sscanf(argv[++i], "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                st.width = w;
                st.height = h;
            } else {
                fprintf(stderr, "invalid --size, expected WxH\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--layer") == 0 && i + 1 < argc) {
            st.layer_monitor = strdup(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if (!url_file) {
        fprintf(stderr, "error: --url-file is required\n");
        usage(argv[0]);
        return 1;
    }

    FILE *f = fopen(url_file, "r");
    if (!f) {
        perror("fopen url-file");
        return 1;
    }
    char url_buf[4096];
    if (!fgets(url_buf, sizeof(url_buf), f)) {
        fprintf(stderr, "error: url-file empty\n");
        fclose(f);
        return 1;
    }
    fclose(f);
    url_buf[strcspn(url_buf, "\r\n")] = '\0';
    st.url = strdup(url_buf);
    if (!st.url || !*st.url) {
        fprintf(stderr, "error: url-file empty\n");
        return 1;
    }
    printf("url-file: %s\n", url_file);
    printf("url length: %zu chars\n", strlen(st.url));

    st.mpv = mpv_create();
    if (!st.mpv) {
        fprintf(stderr, "mpv_create failed\n");
        return 1;
    }

    /* Options before initialize. */
    set_opt(st.mpv, "vo", "libmpv");
    set_opt(st.mpv, "hwdec", st.hwdec);
    set_opt(st.mpv, "profile", "low-latency");
    set_opt(st.mpv, "audio", "no");
    set_opt(st.mpv, "ao", "null");
    set_opt(st.mpv, "cache", "no");
    set_opt(st.mpv, "demuxer-readahead-secs", "0");
    set_opt(st.mpv, "network-timeout", "3");
    set_opt(st.mpv, "rtsp-transport", st.rtsp_transport);
    set_opt(st.mpv, "keep-open", "no");
    set_opt(st.mpv, "idle", "yes");
    set_opt(st.mpv, "force-window", "no");
    set_opt(st.mpv, "msg-level", "all=warn");

    if (st.benchmark) {
        /* Ignore frame display timing; decode/render as fast as possible. */
        set_opt(st.mpv, "untimed", "yes");
    }

    int err = mpv_initialize(st.mpv);
    if (err < 0) {
        fprintf(stderr, "mpv_initialize failed: %s\n", mpv_error_string(err));
        return 1;
    }
    printf("mpv: initialized\n");
    mpv_request_log_messages(st.mpv, "warn");

    /* Observe properties we want to print. */
    mpv_observe_property(st.mpv, 0, "video-params", MPV_FORMAT_NODE);
    mpv_observe_property(st.mpv, 0, "video-codec", MPV_FORMAT_STRING);
    mpv_observe_property(st.mpv, 0, "container-fps", MPV_FORMAT_DOUBLE);
    mpv_observe_property(st.mpv, 0, "hwdec-current", MPV_FORMAT_STRING);

    st.event_thread = g_thread_new("mpv-events", mpv_event_thread, &st);

    GtkApplication *app = gtk_application_new(APP_ID, G_APPLICATION_DEFAULT_FLAGS);
    st.app = app;
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), &st);

    now(&st.t0_run);
    if (st.duration_sec > 0)
        g_timeout_add((guint)(st.duration_sec * 1000.0), on_duration_timeout, &st);
    g_unix_signal_add(SIGINT, on_sigint, &st);

    char *gtk_argv[] = {argv[0], NULL};
    int status = g_application_run(G_APPLICATION(app), 1, gtk_argv);

    /* No-op if a timeout/close/EOF already stopped us. */
    request_stop(&st);

    /* Wait for the mpv event thread before destroying the handle. */
    if (st.event_thread)
        g_thread_join(st.event_thread);

    mpv_terminate_destroy(st.mpv);
    st.mpv = NULL;

    struct timespec t_end;
    now(&t_end);

    printf("--- summary ---\n");
    printf("elapsed (main loop): %.3f s\n", timespec_diff_ms(&st.t0_run, &t_end) / 1000.0);
    printf("frames rendered: %d\n", st.frames_rendered);
    if (st.first_frame_reported && st.frames_rendered > 1) {
        double span = timespec_diff_ms(&st.t0_first_frame, &t_end) / 1000.0;
        if (span > 0)
            printf("render rate after first frame: %.1f fps over %.3f s\n",
                   (st.frames_rendered - 1) / span, span);
    }
    printf("video: codec=%s %dx%d @ %.3f fps pixfmt=%s\n",
           st.video_codec ? st.video_codec : "(unknown)",
           st.video_w, st.video_h, st.container_fps,
           st.video_pixfmt ? st.video_pixfmt : "(unknown)");
    printf("hwdec-current: %s\n", st.hwdec_current ? st.hwdec_current : "(none)");
    if (g_atomic_int_get(&st.setup_failed))
        printf("setup failed\n");
    if (g_atomic_int_get(&st.playback_failed))
        printf("playback failed\n");

    free(st.url);
    free(st.hwdec);
    free(st.rtsp_transport);
    free(st.video_codec);
    free(st.video_pixfmt);
    free(st.hwdec_current);

    if (g_atomic_int_get(&st.setup_failed))
        return 2;
    if (g_atomic_int_get(&st.playback_failed) && st.frames_rendered == 0)
        return 3;
    return status;
}
