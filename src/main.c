#define _GNU_SOURCE
#include "app.h"
#include "config.h"
#include "ipc.h"
#include "logging.h"
#include "overlay.h"
#include "util.h"

#include <fcntl.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <math.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { EXIT_OK = 0, EXIT_FAILED = 1, EXIT_USAGE = 2, EXIT_UNAVAILABLE = 3, EXIT_TIMEOUT = 4 };

static char *opt_config, *opt_socket;
static gboolean opt_no_spawn, opt_json, opt_no_overlay, opt_version;

static const GOptionEntry ENTRIES[] = {
	{ "config", 'c', 0, G_OPTION_ARG_FILENAME, &opt_config, "Config file (default: $XDG_CONFIG_HOME/cctv-overlay/config.ini)", "PATH" },
	{ "socket", 0, 0, G_OPTION_ARG_FILENAME, &opt_socket, "Control socket path (default: $XDG_RUNTIME_DIR/cctv-overlay/control.sock)", "PATH" },
	{ "no-spawn", 0, 0, G_OPTION_ARG_NONE, &opt_no_spawn, "Do not start the daemon automatically", NULL },
	{ "json", 0, 0, G_OPTION_ARG_NONE, &opt_json, "Print the raw JSON reply", NULL },
	{ "no-overlay", 0, 0, G_OPTION_ARG_NONE, &opt_no_overlay, "daemon: run without any overlay window", NULL },
	{ "version", 0, 0, G_OPTION_ARG_NONE, &opt_version, "Print version and exit", NULL },
	{ NULL }
};

/* ---------------- daemon ---------------- */

static GMainLoop *g_loop;

static void quit_loop(void *user)
{
	(void)user;
	g_main_loop_quit(g_loop);
}

static gboolean on_signal(gpointer data)
{
	log_info("received signal %d, shutting down", GPOINTER_TO_INT(data));
	g_main_loop_quit(g_loop);
	return G_SOURCE_REMOVE;
}

static int run_daemon(void)
{
	/* The daemon holds the RTSP password in memory; never let a crash write it into a core dump.
	 * systemd units can set LimitCORE=0 too, but an auto-started daemon has no unit. */
	struct rlimit no_core = { 0, 0 };
	setrlimit(RLIMIT_CORE, &no_core);

	GError *e = NULL;
	Config *cfg = config_load(opt_config, &e);

	if (!cfg) {
		fprintf(stderr, "cctv-overlay: configuration error: %s\n", e->message);
		g_error_free(e);
		return EXIT_USAGE;
	}
	log_set_level(cfg->log_level);

	/* Real build: opens the Wayland display and verifies layer-shell; fail loudly if it cannot. */
	if (!opt_no_overlay && !overlay_init(&e)) {
		fprintf(stderr, "cctv-overlay: overlay unavailable: %s\n", e->message);
		g_error_free(e);
		config_free(cfg);
		return EXIT_FAILED;
	}

	double scale = 1.0;
	const char *sc = g_getenv("CCTV_OVERLAY_BACKOFF_SCALE"); /* test knob */
	if (sc && *sc) {
		scale = g_ascii_strtod(sc, NULL);
		if (!isfinite(scale) || scale <= 0 || scale > 600)
			scale = 1.0;
	}

	g_loop = g_main_loop_new(NULL, FALSE);
	AppOptions ao = { .overlay_enabled = !opt_no_overlay, .backoff_scale = scale, .quit_cb = quit_loop };
	App *app = app_new(cfg, &ao);

	IpcServer *srv = ipc_server_new(opt_socket, app_ipc_handler, app_ipc_after_reply, app, &e);
	if (!srv) {
		fprintf(stderr, "cctv-overlay: %s\n", e->message);
		g_error_free(e);
		app_free(app);
		g_main_loop_unref(g_loop);
		return EXIT_FAILED;
	}
	signal(SIGPIPE, SIG_IGN);
	g_unix_signal_add(SIGTERM, on_signal, GINT_TO_POINTER(SIGTERM));
	g_unix_signal_add(SIGINT, on_signal, GINT_TO_POINTER(SIGINT));

	log_info("cctv-overlay %s daemon started (%u cameras, hidden)", CCTV_VERSION, cfg->cameras->len);
	g_main_loop_run(g_loop);

	app_free(app);
	ipc_server_free(srv);
	g_main_loop_unref(g_loop);
	log_info("daemon stopped");
	return EXIT_OK;
}

/* ---------------- client ---------------- */

static char *build_request(const char *cmd, const char *arg)
{
	JsonBuilder *b = json_builder_new();
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "cmd");
	json_builder_add_string_value(b, cmd);
	if (arg) {
		json_builder_set_member_name(b, "camera");
		json_builder_add_string_value(b, arg);
	}
	json_builder_end_object(b);
	JsonGenerator *g = json_generator_new();
	JsonNode *root = json_builder_get_root(b);
	json_generator_set_root(g, root);
	char *s = json_generator_to_data(g, NULL);
	json_node_free(root);
	g_object_unref(g);
	g_object_unref(b);
	return s;
}

static void detach_child(gpointer unused)
{
	(void)unused;
	setsid();
}

static gboolean spawn_daemon(const char *socket_path, GError **err)
{
	char *self = g_file_read_link("/proc/self/exe", NULL);
	GPtrArray *argv = g_ptr_array_new();

	g_ptr_array_add(argv, self ? self : (char *)"cctv-overlay");
	g_ptr_array_add(argv, (char *)"daemon");
	if (opt_config) {
		g_ptr_array_add(argv, (char *)"--config");
		g_ptr_array_add(argv, opt_config);
	}
	if (opt_socket) {
		g_ptr_array_add(argv, (char *)"--socket");
		g_ptr_array_add(argv, (char *)socket_path);
	}
	if (opt_no_overlay)
		g_ptr_array_add(argv, (char *)"--no-overlay");
	g_ptr_array_add(argv, NULL);

	/* An auto-started daemon has no terminal, so its (already redacted) log goes to a private file:
	 * $XDG_STATE_HOME/cctv-overlay/daemon.log, mode 0600, rotated once above 512 KiB. */
	int logfd = -1;
	char *dir = g_build_filename(g_get_user_state_dir(), "cctv-overlay", NULL);
	if (g_mkdir_with_parents(dir, 0700) == 0) {
		char *path = g_build_filename(dir, "daemon.log", NULL);
		struct stat st;
		if (stat(path, &st) == 0 && st.st_size > 512 * 1024) {
			char *old = g_strconcat(path, ".1", NULL);
			rename(path, old);
			g_free(old);
		}
		logfd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
		if (logfd >= 0) {
			fchmod(logfd, 0600); /* tighten a pre-existing looser file */
			chmod(dir, 0700);
		}
		g_free(path);
	}
	g_free(dir);

	/* Config path only on argv; secrets stay in files/environment. */
	gboolean ok = g_spawn_async_with_pipes_and_fds(NULL, (const char *const *)argv->pdata, NULL,
						       G_SPAWN_STDIN_FROM_DEV_NULL | (logfd < 0 ? (G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL) : 0),
						       detach_child, NULL, -1, logfd, logfd, NULL, NULL, 0, NULL, NULL,
						       NULL, NULL, err);
	if (logfd >= 0)
		close(logfd);
	g_ptr_array_free(argv, TRUE);
	g_free(self);
	return ok;
}

static void print_status_human(JsonObject *o)
{
	const char *state = json_object_get_string_member(o, "state");
	printf("%s: %s (camera %" G_GINT64_FORMAT " of %" G_GINT64_FORMAT ", zero-based index)\n", state,
	       json_object_get_string_member(o, "camera"), json_object_get_int_member(o, "camera_index"),
	       json_object_get_int_member(o, "camera_count"));
	if (!json_object_get_null_member(o, "error"))
		printf("error: %s\n", json_object_get_string_member(o, "error"));
}

static void print_human(const char *cmd, JsonObject *o)
{
	if (strcmp(cmd, "status") == 0) {
		print_status_human(o);
	} else if (strcmp(cmd, "list") == 0) {
		JsonArray *a = json_object_get_array_member(o, "cameras");
		for (guint i = 0; i < json_array_get_length(a); i++) {
			JsonObject *c = json_array_get_object_element(a, i);
			printf("%c %" G_GINT64_FORMAT "  %-16s %s (channel %" G_GINT64_FORMAT ")\n",
			       json_object_get_boolean_member(c, "current") ? '*' : ' ',
			       json_object_get_int_member(c, "index"), json_object_get_string_member(c, "id"),
			       json_object_get_string_member(c, "name"), json_object_get_int_member(c, "channel"));
		}
	}
}

static int run_client(const char *cmd, const char *arg)
{
	GError *e = NULL;
	char *sock = opt_socket ? g_strdup(opt_socket) : ipc_default_socket_path(&e);

	if (!sock) {
		fprintf(stderr, "cctv-overlay: %s\n", e->message);
		g_error_free(e);
		return EXIT_USAGE;
	}
	char *req = build_request(cmd, arg);
	char *reply = NULL;
	IpcResult r = ipc_client_request(sock, req, &reply, &e);

	if (r == IPC_UNAVAILABLE && !opt_no_spawn && strcmp(cmd, "quit") != 0) {
		g_clear_error(&e);
		/* Catch config errors here, with a real message, instead of a silent daemon exit. */
		Config *cfg = config_load(opt_config, &e);
		if (!cfg) {
			fprintf(stderr, "cctv-overlay: configuration error: %s\n", e->message);
			g_error_free(e);
			return EXIT_USAGE;
		}
		config_free(cfg);
		if (!spawn_daemon(sock, &e)) {
			fprintf(stderr, "cctv-overlay: cannot start the daemon: %s\n", e->message);
			g_error_free(e);
			return EXIT_UNAVAILABLE;
		}
		gint64 deadline = util_monotonic_ms() + 2000;
		do {
			g_usleep(25 * 1000);
			g_clear_error(&e);
			r = ipc_client_request(sock, req, &reply, &e);
		} while (r == IPC_UNAVAILABLE && util_monotonic_ms() < deadline);
	}

	int rc = EXIT_FAILED;
	if (r == IPC_UNAVAILABLE) {
		fprintf(stderr, "cctv-overlay: daemon is not running%s\n",
			opt_no_spawn || strcmp(cmd, "quit") == 0 ? "" :
			" and did not start within 2 s (run 'cctv-overlay daemon' in a terminal to see why)");
		rc = EXIT_UNAVAILABLE;
	} else if (r == IPC_TIMEOUT) {
		fprintf(stderr, "cctv-overlay: timed out waiting for the daemon\n");
		rc = EXIT_TIMEOUT;
	} else if (r == IPC_FAILED) {
		fprintf(stderr, "cctv-overlay: %s\n", e ? e->message : "request failed");
	} else {
		JsonParser *p = json_parser_new();
		if (json_parser_load_from_data(p, reply, -1, NULL) && JSON_NODE_HOLDS_OBJECT(json_parser_get_root(p))) {
			JsonObject *o = json_node_get_object(json_parser_get_root(p));
			gboolean ok = json_object_has_member(o, "ok") && json_object_get_boolean_member(o, "ok");
			if (opt_json)
				printf("%s\n", reply);
			if (ok) {
				if (!opt_json)
					print_human(cmd, o);
				rc = EXIT_OK;
			} else if (!opt_json) {
				const char *m = json_object_has_member(o, "error") ? json_object_get_string_member(o, "error") : "command failed";
				fprintf(stderr, "cctv-overlay: %s\n", m);
			}
		} else {
			fprintf(stderr, "cctv-overlay: unreadable reply from the daemon\n");
		}
		g_object_unref(p);
	}
	if (e)
		g_error_free(e);
	g_free(reply);
	g_free(req);
	g_free(sock);
	return rc;
}

/* ---------------- entry ---------------- */

int main(int argc, char **argv)
{
	GError *e = NULL;
	GOptionContext *ctx = g_option_context_new("COMMAND [CAMERA]");

	g_option_context_set_summary(ctx,
		"Commands:\n"
		"  daemon           run the overlay daemon in the foreground\n"
		"  toggle           show/hide the overlay\n"
		"  next | prev      cycle cameras (opens the overlay if hidden)\n"
		"  show [CAMERA]    open the overlay on CAMERA (name, id or zero-based index)\n"
		"  hide             close the overlay and free all resources\n"
		"  status [--json]  print the current state\n"
		"  list             list configured cameras\n"
		"  reload           re-read the configuration\n"
		"  quit             stop the daemon\n"
		"  ping             check that the daemon answers\n"
		"\nExit codes: 0 ok, 1 command failed, 2 usage/config error, 3 daemon unavailable, 4 timeout");
	g_option_context_add_main_entries(ctx, ENTRIES, NULL);
	if (!g_option_context_parse(ctx, &argc, &argv, &e)) {
		fprintf(stderr, "cctv-overlay: %s\n", e->message);
		return EXIT_USAGE;
	}
	if (opt_version) {
		printf("cctv-overlay %s\n", CCTV_VERSION);
		return EXIT_OK;
	}
	if (argc < 2) {
		char *help = g_option_context_get_help(ctx, TRUE, NULL);
		fputs(help, stderr);
		return EXIT_USAGE;
	}
	g_option_context_free(ctx);

	const char *cmd = argv[1];
	const char *arg = argc > 2 ? argv[2] : NULL;
	static const char *const SIMPLE[] = { "toggle", "next", "prev", "hide", "status", "list", "reload",
					      "quit", "ping", NULL };

	if (argc > 3 || (arg && strcmp(cmd, "show") != 0)) {
		fprintf(stderr, "cctv-overlay: unexpected argument '%s' for '%s'\n", argv[argc > 3 ? 3 : 2], cmd);
		return EXIT_USAGE;
	}
	if (strcmp(cmd, "daemon") == 0)
		return run_daemon();
	if (strcmp(cmd, "show") == 0)
		return run_client(cmd, arg);
	for (guint i = 0; SIMPLE[i]; i++)
		if (strcmp(cmd, SIMPLE[i]) == 0)
			return run_client(cmd, NULL);

	fprintf(stderr, "cctv-overlay: unknown command '%s' (try --help)\n", cmd);
	return EXIT_USAGE;
}
