#!/usr/bin/env bash
# Try cctv-overlay without any cameras: five synthetic "cameras" (tools/demo/scenes.py) are served by the
# loopback RTSP test server on 127.0.0.1, and a separate demo daemon (own socket and config, so a real setup
# is not touched) shows them. This is how the screenshots and GIFs in docs/media were made.
#
#   tools/demo/run-demo.sh start [MONITOR]    start servers + demo daemon (MONITOR default: focused)
#   tools/demo/run-demo.sh ctl toggle|next|prev|audio|status ...   control the demo daemon
#   tools/demo/run-demo.sh stop
#
# Needs: ffmpeg, python3, a built ./cctv-overlay. Credentials are fake (demo/demo) and loopback-only.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
RUN=${XDG_RUNTIME_DIR:?needs XDG_RUNTIME_DIR}/cctv-overlay-demo
SOCK=$RUN/control.sock
BIN=${CCTV_OVERLAY_BIN:-$ROOT/cctv-overlay}
BASE_PORT=${CCTV_DEMO_PORT:-18801}
SCENES=(front driveway backyard garage alley)
NAMES=("Front Door" "Driveway" "Backyard" "Garage" "Side Alley")

ctl() { "$BIN" --socket "$SOCK" --config "$RUN/config.ini" --no-spawn "$@"; }

start() {
	local mon=${1:-focused} i
	mkdir -p "$RUN"; chmod 700 "$RUN"
	{
		echo "[general]"; echo "monitor = $mon"
		echo "[overlay]"; echo "width = 720"; echo "height = 405"; echo "anchor = top-right"
		echo "[stream]"; echo "subtype = 0"
		echo "[recorder]"; echo "host = 127.0.0.1"; echo "username = demo"
		for i in "${!SCENES[@]}"; do
			echo "[camera.${SCENES[$i]}]"; echo "name = ${NAMES[$i]}"
			echo "port = $((BASE_PORT + i))"; echo "path = /live"
		done
	} >"$RUN/config.ini"
	(umask 077; printf '[recorder]\npassword = demo\n' >"$RUN/secrets.ini")
	for i in "${!SCENES[@]}"; do
		mapfile -t L < <(python3 -I "$ROOT/tools/demo/scenes.py" "${SCENES[$i]}")
		python3 -I "$ROOT/tests/tools/rtsp_loopback.py" --port $((BASE_PORT + i)) --user demo --password demo \
			--size 960x540 --gop 12 --audio --pattern "${L[0]}" --vf "${L[1]}" >"$RUN/srv$i.log" 2>&1 &
		echo $! >"$RUN/srv$i.pid"
	done
	sleep 1.5
	XDG_STATE_HOME="$RUN/state" "$BIN" --socket "$SOCK" daemon --config "$RUN/config.ini" >"$RUN/daemon.log" 2>&1 &
	echo $! >"$RUN/daemon.pid"
	for i in $(seq 50); do [ -S "$SOCK" ] && break; sleep 0.1; done
	[ -S "$SOCK" ] || { cat "$RUN/daemon.log"; exit 1; }
	echo "demo running on monitor '$mon'. Try: $0 ctl toggle"
}

stop() {
	ctl quit >/dev/null 2>&1 || true
	for f in "$RUN"/srv*.pid "$RUN"/daemon.pid; do [ -f "$f" ] && kill "$(cat "$f")" 2>/dev/null || true; rm -f "$f"; done
	echo "demo stopped"
}

case "${1:-}" in
start) shift; start "$@" ;;
stop) stop ;;
ctl) shift; ctl "$@" ;;
*) sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
