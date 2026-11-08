#define _GNU_SOURCE
#include "ipc.h"

#include "logging.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <gio/gunixsocketaddress.h>
#include <string.h>
#include <sys/file.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>

struct IpcServer {
	GSocketService *service;
	char *path;
	int lock_fd;
	IpcHandler handler;
	IpcAfterReply after;
	void *user;
};

typedef struct {
	IpcServer *srv;
	GSocketConnection *conn;
	GCancellable *cancel;
	guint timeout_src;
	char buf[IPC_MAX_REQUEST + 1];
	gsize len;
	char *cmd;
} Conn;

char *ipc_default_socket_path(GError **err)
{
	const char *rt = g_getenv("XDG_RUNTIME_DIR");
	if (!rt || !*rt) {
		g_set_error_literal(err, G_IO_ERROR, G_IO_ERROR_FAILED,
				    "XDG_RUNTIME_DIR is not set; use --socket PATH");
		return NULL;
	}
	return g_build_filename(rt, "cctv-overlay", "control.sock", NULL);
}

/* ---------------- server ---------------- */

static void conn_free(Conn *c)
{
	if (c->timeout_src)
		g_source_remove(c->timeout_src);
	g_cancellable_cancel(c->cancel);
	g_io_stream_close(G_IO_STREAM(c->conn), NULL, NULL);
	g_object_unref(c->conn);
	g_object_unref(c->cancel);
	g_free(c->cmd);
	g_free(c);
}

static char *serialize(JsonObject *obj)
{
	JsonNode *root = json_node_new(JSON_NODE_OBJECT);
	json_node_set_object(root, obj);
	JsonGenerator *gen = json_generator_new();
	json_generator_set_root(gen, root);
	char *s = json_generator_to_data(gen, NULL);
	g_object_unref(gen);
	json_node_free(root);
	char *line = g_strconcat(s, "\n", NULL);
	g_free(s);
	return line;
}

static JsonObject *error_reply(const char *msg)
{
	JsonObject *o = json_object_new();
	json_object_set_boolean_member(o, "ok", FALSE);
	json_object_set_string_member(o, "error", msg);
	return o;
}

static void write_done(GObject *src, GAsyncResult *res, gpointer data)
{
	Conn *c = data;
	GError *e = NULL;
	g_output_stream_write_all_finish(G_OUTPUT_STREAM(src), res, NULL, &e);
	if (e) {
		log_debug("ipc: reply write failed: %s", e->message);
		g_error_free(e);
	}
	if (c->srv->after && c->cmd)
		c->srv->after(c->srv->user, c->cmd);
	conn_free(c);
}

static void send_reply(Conn *c, JsonObject *reply)
{
	if (!json_object_has_member(reply, "version"))
		json_object_set_string_member(reply, "version", CCTV_VERSION);
	if (json_object_has_member(reply, "error") &&
	    json_object_get_member(reply, "error") &&
	    JSON_NODE_HOLDS_VALUE(json_object_get_member(reply, "error")) &&
	    json_object_get_string_member(reply, "error")) {
		char *safe = log_redact(json_object_get_string_member(reply, "error"));
		json_object_set_string_member(reply, "error", safe);
		g_free(safe);
	}
	char *line = serialize(reply);
	json_object_unref(reply);

	GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(c->conn));
	/* tiny reply: ownership of `line` passes via the bytes object */
	GBytes *bytes = g_bytes_new_take(line, strlen(line));
	gsize n;
	const void *data = g_bytes_get_data(bytes, &n);
	g_object_set_data_full(G_OBJECT(c->conn), "reply-bytes", bytes, (GDestroyNotify)g_bytes_unref);
	g_output_stream_write_all_async(out, data, n, G_PRIORITY_DEFAULT, c->cancel, write_done, c);
}

static void process_line(Conn *c, const char *line)
{
	JsonParser *p = json_parser_new();
	JsonObject *reply;

	if (!json_parser_load_from_data(p, line, -1, NULL) || !json_parser_get_root(p) ||
	    !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(p))) {
		reply = error_reply("malformed request: expected one JSON object");
	} else {
		JsonObject *req = json_node_get_object(json_parser_get_root(p));
		JsonNode *cn = json_object_get_member(req, "cmd");
		if (!cn || !JSON_NODE_HOLDS_VALUE(cn) || json_node_get_value_type(cn) != G_TYPE_STRING) {
			reply = error_reply("malformed request: missing string member \"cmd\"");
		} else {
			const char *cmd = json_node_get_string(cn);
			c->cmd = g_strdup(cmd);
			reply = c->srv->handler(c->srv->user, cmd, req);
		}
	}
	g_object_unref(p);
	send_reply(c, reply);
}

static void read_more(Conn *c);

static void read_done(GObject *src, GAsyncResult *res, gpointer data)
{
	Conn *c = data;
	GError *e = NULL;
	gssize n = g_input_stream_read_finish(G_INPUT_STREAM(src), res, &e);

	if (n < 0) {
		g_clear_error(&e); /* cancelled by timeout or client reset */
		conn_free(c);
		return;
	}
	if (n == 0) { /* EOF before a full line */
		conn_free(c);
		return;
	}
	gsize old = c->len;
	c->len += (gsize)n;
	char *nl = memchr(c->buf + old, '\n', (gsize)n);
	if (nl) {
		*nl = '\0';
		if (c->timeout_src) {
			g_source_remove(c->timeout_src);
			c->timeout_src = 0;
		}
		process_line(c, c->buf);
		return;
	}
	if (c->len >= IPC_MAX_REQUEST) {
		if (c->timeout_src) {
			g_source_remove(c->timeout_src);
			c->timeout_src = 0;
		}
		send_reply(c, error_reply("request too large (limit 4096 bytes)"));
		return;
	}
	read_more(c);
}

static void read_more(Conn *c)
{
	GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(c->conn));
	g_input_stream_read_async(in, c->buf + c->len, IPC_MAX_REQUEST - c->len, G_PRIORITY_DEFAULT, c->cancel,
				  read_done, c);
}

static gboolean conn_timeout(gpointer data)
{
	Conn *c = data;
	c->timeout_src = 0;
	log_debug("ipc: dropping idle connection (%d s timeout)", IPC_TIMEOUT_S);
	/* A read is pending: cancelling makes read_done() run with an error and free the connection. */
	g_cancellable_cancel(c->cancel);
	return G_SOURCE_REMOVE;
}

static gboolean on_incoming(GSocketService *svc, GSocketConnection *conn, GObject *src, gpointer data)
{
	(void)svc;
	(void)src;
	IpcServer *s = data;
	GError *e = NULL;
	GSocket *sock = g_socket_connection_get_socket(conn);
	GCredentials *cred = g_socket_get_credentials(sock, &e);

	GCredentials *self = g_credentials_new();
	gboolean same = cred && g_credentials_is_same_user(cred, self, NULL);
	g_object_unref(self);
	if (!same) {
		log_warn("ipc: rejected connection from a different user or with unreadable credentials");
		g_clear_error(&e);
		if (cred)
			g_object_unref(cred);
		return TRUE; /* GIO closes the connection when we drop our ref */
	}
	g_object_unref(cred);

	Conn *c = g_new0(Conn, 1);
	c->srv = s;
	c->conn = g_object_ref(conn);
	c->cancel = g_cancellable_new();
	c->timeout_src = g_timeout_add_seconds(IPC_TIMEOUT_S, conn_timeout, c);
	read_more(c);
	return TRUE;
}

static gboolean ensure_dir(const char *dir, GError **err)
{
	if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
		g_set_error(err, G_IO_ERROR, G_IO_ERROR_FAILED, "cannot create %s: %s", dir, g_strerror(errno));
		return FALSE;
	}
	struct stat st;
	if (lstat(dir, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid()) {
		g_set_error(err, G_IO_ERROR, G_IO_ERROR_FAILED, "%s is not a directory owned by you", dir);
		return FALSE;
	}
	if ((st.st_mode & 077) && chmod(dir, 0700) != 0) {
		g_set_error(err, G_IO_ERROR, G_IO_ERROR_FAILED, "cannot set mode 0700 on %s: %s", dir,
			    g_strerror(errno));
		return FALSE;
	}
	return TRUE;
}

IpcServer *ipc_server_new(const char *socket_path, IpcHandler handler, IpcAfterReply after, void *user,
			  GError **err)
{
	char *path = NULL, *lock_path = NULL;

	if (socket_path) {
		path = g_strdup(socket_path);
		lock_path = g_strconcat(path, ".lock", NULL);
	} else {
		path = ipc_default_socket_path(err);
		if (!path)
			return NULL;
		char *dir = g_path_get_dirname(path);
		gboolean ok = ensure_dir(dir, err);
		lock_path = g_build_filename(dir, "daemon.lock", NULL);
		g_free(dir);
		if (!ok) {
			g_free(path);
			g_free(lock_path);
			return NULL;
		}
	}
	if (strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
		g_set_error(err, G_IO_ERROR, G_IO_ERROR_FAILED, "socket path too long: %s", path);
		goto fail;
	}

	int fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (fd < 0) {
		g_set_error(err, G_IO_ERROR, G_IO_ERROR_FAILED, "cannot open lock file %s: %s", lock_path,
			    g_strerror(errno));
		goto fail;
	}
	if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
		g_set_error(err, G_IO_ERROR, G_IO_ERROR_BUSY,
			    "another cctv-overlay daemon is already running (lock held on %s); use "
			    "'cctv-overlay quit' to stop it",
			    lock_path);
		close(fd);
		goto fail;
	}

	unlink(path); /* stale socket from a crashed daemon; we hold the lock */
	IpcServer *s = g_new0(IpcServer, 1);
	s->path = path;
	s->lock_fd = fd;
	s->handler = handler;
	s->after = after;
	s->user = user;
	s->service = g_socket_service_new();

	GSocketAddress *addr = g_unix_socket_address_new(path);
	mode_t old = umask(0177);
	gboolean ok = g_socket_listener_add_address(G_SOCKET_LISTENER(s->service), addr, G_SOCKET_TYPE_STREAM,
						    G_SOCKET_PROTOCOL_DEFAULT, NULL, NULL, err);
	umask(old);
	g_object_unref(addr);
	if (!ok || chmod(path, 0600) != 0) {
		if (ok)
			g_set_error(err, G_IO_ERROR, G_IO_ERROR_FAILED, "cannot chmod %s: %s", path,
				    g_strerror(errno));
		g_object_unref(s->service);
		close(fd);
		g_free(s);
		g_free(lock_path);
		g_free(path);
		return NULL;
	}
	g_signal_connect(s->service, "incoming", G_CALLBACK(on_incoming), s);
	g_socket_service_start(s->service);
	g_free(lock_path);
	log_info("control socket listening on %s", path);
	return s;

fail:
	g_free(path);
	g_free(lock_path);
	return NULL;
}

void ipc_server_free(IpcServer *s)
{
	if (!s)
		return;
	g_socket_service_stop(s->service);
	g_socket_listener_close(G_SOCKET_LISTENER(s->service));
	g_object_unref(s->service);
	unlink(s->path);
	close(s->lock_fd); /* releases the flock */
	g_free(s->path);
	g_free(s);
}

const char *ipc_server_path(const IpcServer *s) { return s->path; }

/* ---------------- client ---------------- */

IpcResult ipc_client_request(const char *socket_path, const char *request, char **reply, GError **err)
{
	GSocketClient *cl = g_socket_client_new();
	GSocketAddress *addr = g_unix_socket_address_new(socket_path);
	GError *e = NULL;
	IpcResult res = IPC_FAILED;

	g_socket_client_set_timeout(cl, IPC_TIMEOUT_S);
	GSocketConnection *conn = g_socket_client_connect(cl, G_SOCKET_CONNECTABLE(addr), NULL, &e);
	if (!conn) {
		res = g_error_matches(e, G_IO_ERROR, G_IO_ERROR_TIMED_OUT) ? IPC_TIMEOUT : IPC_UNAVAILABLE;
		g_propagate_error(err, e);
		goto out;
	}

	GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
	char *line = g_strconcat(request, "\n", NULL);
	gboolean w = g_output_stream_write_all(out, line, strlen(line), NULL, NULL, &e);
	g_free(line);
	if (w) {
		GDataInputStream *din = g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(conn)));
		gsize len = 0;
		*reply = g_data_input_stream_read_line(din, &len, NULL, &e);
		g_object_unref(din);
		if (*reply)
			res = IPC_OK;
		else if (!e)
			g_set_error_literal(&e, G_IO_ERROR, G_IO_ERROR_FAILED, "daemon closed the connection without a reply");
	}
	if (res != IPC_OK) {
		if (e && g_error_matches(e, G_IO_ERROR, G_IO_ERROR_TIMED_OUT))
			res = IPC_TIMEOUT;
		g_propagate_error(err, e);
	}
	g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
	g_object_unref(conn);
out:
	g_object_unref(addr);
	g_object_unref(cl);
	return res;
}
