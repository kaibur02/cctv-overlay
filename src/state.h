#ifndef CCTV_STATE_H
#define CCTV_STATE_H

typedef enum {
	APP_HIDDEN = 0,
	APP_CONNECTING,
	APP_LIVE,
	APP_RECONNECTING,
	APP_ERROR
} AppState;

static inline const char *app_state_name(AppState s)
{
	switch (s) {
	case APP_HIDDEN: return "hidden";
	case APP_CONNECTING: return "connecting";
	case APP_LIVE: return "live";
	case APP_RECONNECTING: return "reconnecting";
	case APP_ERROR: return "error";
	}
	return "unknown";
}

#endif
