#ifndef CCTV_IPC_H
#define CCTV_IPC_H

#include <gio/gio.h>
#include <json-glib/json-glib.h>

#define IPC_MAX_REQUEST 4096
#define IPC_TIMEOUT_S 2

/* Handler returns a new JsonObject reply (must contain "ok"); ipc adds "version" and redacts
 * the "error" member. `req` is the parsed request object (valid only during the call). */
typedef JsonObject *(*IpcHandler)(void *user, const char *cmd, JsonObject *req);
/* Called after the reply for `cmd` has been written (or failed). Used for "quit". */
typedef void (*IpcAfterReply)(void *user, const char *cmd);

typedef struct IpcServer IpcServer;

/* socket_path NULL -> $XDG_RUNTIME_DIR/cctv-overlay/control.sock (dir 0700). Takes the
 * daemon.lock flock (single instance), removes a stale socket, binds with mode 0600. */
IpcServer *ipc_server_new(const char *socket_path, IpcHandler handler, IpcAfterReply after, void *user,
			  GError **err);
/* Removes the socket file and releases the lock. */
void ipc_server_free(IpcServer *s);
const char *ipc_server_path(const IpcServer *s);

char *ipc_default_socket_path(GError **err);

typedef enum { IPC_OK = 0, IPC_UNAVAILABLE, IPC_TIMEOUT, IPC_FAILED } IpcResult;

/* Send one request line (without newline) and read one reply line. */
IpcResult ipc_client_request(const char *socket_path, const char *request, char **reply, GError **err);

#endif
