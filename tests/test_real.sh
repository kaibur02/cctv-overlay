#!/usr/bin/env bash
# Opt-in integration test for the REAL build (`make check-real`): GTK4 layer-shell overlay + libmpv.
#
# Needs a running Wayland session (Hyprland: uses hyprctl for layer/client checks), grim, jq, ffmpeg,
# python3, ss. It shows the overlay briefly on ONE monitor (CCTV_TEST_MONITOR, default DP-1) and
# never sends keystrokes, pointer input or focus changes. Network: loopback only (tests/tools/
# rtsp_loopback.py serves a synthetic stream); credentials are fake. Does not read ~/.config/cctv-overlay.
#
# Usage: tests/test_real.sh [path-to-cctv-overlay]
# Env:   CCTV_TEST_MONITOR (DP-1)  CCTV_TEST_PORT (18554; uses PORT and PORT+1)  CCTV_TEST_CYCLES (20)
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
BIN=$(readlink -f "${1:-./cctv-overlay}")
MON=${CCTV_TEST_MONITOR:-DP-1}
PORT=${CCTV_TEST_PORT:-18554}
PORT2=$((PORT + 1))
CYCLES=${CCTV_TEST_CYCLES:-20}

skip() { echo "SKIP: $1"; exit 0; }
[ -n "${WAYLAND_DISPLAY:-}" ] || skip "no Wayland session"
for t in hyprctl grim jq ffmpeg python3 ss; do command -v "$t" >/dev/null || skip "$t not installed"; done
hyprctl monitors -j | jq -e --arg m "$MON" '.[]|select(.name==$m)' >/dev/null || skip "monitor $MON not present"

PW='p@ss:w/rd%1'
PW_ENC='p%40ss%3Aw%2Frd%251'
T=$(mktemp -d "${TMPDIR:-/tmp}/cctv-real-XXXXXX")
chmod 700 "$T"
SOCK="$T/ctl.sock"
CFG="$T/config.ini"
export XDG_STATE_HOME="$T/state"
export CCTV_OVERLAY_BACKOFF_SCALE=0.05
unset CCTV_OVERLAY_PASSWORD CCTV_OVERLAY_USERNAME

cat > "$CFG" <<EOF
[general]
log_level = debug
monitor = $MON
[overlay]
width = 480
height = 270
anchor = top-right
[camera.a]
name = Loop A
url = rtsp://tester:$PW_ENC@127.0.0.1:$PORT/live
[camera.b]
name = Loop B
url = rtsp://tester:$PW_ENC@127.0.0.1:$PORT2/live
[camera.bad]
name = Bad Password
url = rtsp://tester:wrong@127.0.0.1:$PORT/live
[camera.down]
name = Nothing Listens
url = rtsp://tester:$PW_ENC@127.0.0.1:$((PORT + 7))/live
EOF

fails=0 checks=0 DPID="" SPIDS=""
cleanup() {
	[ -n "$DPID" ] && kill "$DPID" 2>/dev/null
	[ -n "$SPIDS" ] && kill $SPIDS 2>/dev/null
	sleep 0.2 # the servers' ffmpeg children exit on EPIPE once their parent is gone
	rm -rf "$T"
}
trap cleanup EXIT
ok()   { checks=$((checks+1)); echo "ok   - $1"; }
fail() { checks=$((checks+1)); fails=$((fails+1)); echo "FAIL - $1"; }
expect_eq() { if [ "$2" = "$3" ]; then ok "$1"; else fail "$1 (got '$2', want '$3')"; fi; }
C() { "$BIN" --socket "$SOCK" --config "$CFG" --no-spawn "$@"; }
jst() { C status --json | jq -r "$1"; }
wait_state() { # wait_state <state> <seconds>
	local n=$(( $2 * 20 ))
	while [ $n -gt 0 ]; do [ "$(jst .state 2>/dev/null)" = "$1" ] && return 0; sleep 0.05; n=$((n-1)); done
	return 1
}
layer_ns() { hyprctl layers -j | jq -r --arg m "$MON" '[.[$m].levels|to_entries[]|.value[]|select(.namespace=="cctv-overlay")]|length'; }
layer_level() { hyprctl layers -j | jq -r --arg m "$MON" '[.[$m].levels|to_entries[]|select(.value|map(.namespace)|index("cctv-overlay"))|.key]|first // "none"'; }
clients_with_overlay() { hyprctl clients -j | jq '[.[]|select((.class+.title)|test("cctv";"i"))]|length'; }
pfd() { ls "/proc/$DPID/fd" | wc -l; }
pthr() { ls "/proc/$DPID/task" | wc -l; }
prss() { awk '/VmRSS/{print $2}' "/proc/$DPID/status"; }
established() { ss -Htn state established "( dport = :$PORT or dport = :$PORT2 )" | wc -l; }
first_frames() { grep -o 'first frame [0-9]* ms' "$T/err.log" | grep -o '[0-9]*'; }

# ---- servers + daemon ----
cd "$HERE/.." || exit 1
msg=$(WAYLAND_DISPLAY=cctv-no-such-display "$BIN" --socket "$T/x.sock" daemon --config "$CFG" 2>&1); rc=$?
expect_eq "no Wayland display -> daemon exits 1 (loud failure, no silent fallback)" "$rc" 1
case "$msg" in *"overlay unavailable"*) ok "failure message names the overlay problem";; *) fail "unclear message: $msg";; esac
python3 -I tests/tools/rtsp_loopback.py --port $PORT --user tester --password "$PW" --gop 5 >"$T/srvA.log" 2>&1 & SPIDS="$!"
python3 -I tests/tools/rtsp_loopback.py --port $PORT2 --user tester --password "$PW" --gop 5 --pattern testsrc2 >"$T/srvB.log" 2>&1 & SPIDS="$SPIDS $!"
for _ in $(seq 100); do ss -Htln "( sport = :$PORT )" | grep -q . && ss -Htln "( sport = :$PORT2 )" | grep -q . && break; sleep 0.05; done

"$BIN" --socket "$SOCK" daemon --config "$CFG" >"$T/out.log" 2>"$T/err.log" & DPID=$!
for _ in $(seq 100); do [ -S "$SOCK" ] && break; sleep 0.05; done
[ -S "$SOCK" ] && ok "daemon started (real build, overlay init)" || { fail "daemon did not start"; cat "$T/err.log"; exit 1; }
expect_eq "no layer surface while hidden" "$(layer_ns)" 0
FD0=$(pfd); TH0=$(pthr)

# ---- show / layer / pixels ----
ACTIVE_BEFORE=$(hyprctl activewindow -j | jq -r '.address // "none"')
C toggle >/dev/null; expect_eq "toggle exits 0" $? 0
wait_state live 10 && ok "reaches live over loopback RTSP (Digest auth, percent-encoded password)" || fail "did not reach live: $(jst .error)"
expect_eq "layer namespace cctv-overlay present on $MON" "$(layer_ns)" 1
expect_eq "layer is at level 3 (overlay)" "$(layer_level)" 3
expect_eq "overlay is not a client window" "$(clients_with_overlay)" 0
expect_eq "active window unchanged by showing" "$(hyprctl activewindow -j | jq -r '.address // "none"')" "$ACTIVE_BEFORE"
grep -q "decoder 'vaapi'" "$T/err.log" && ok "VA-API hardware decode active" || echo "note - decoder is not vaapi (see log)"

GEOM=$(hyprctl layers -j | jq -r --arg m "$MON" '[.[$m].levels|to_entries[]|.value[]|select(.namespace=="cctv-overlay")][0]|"\(.x),\(.y) \(.w)x\(.h)"')
sleep 0.5
grim -g "$GEOM" -t ppm "$T/shot.ppm" && python3 -I - "$T/shot.ppm" <<'PY'
import sys
d = open(sys.argv[1], "rb").read()
parts = d.split(None, 4)
w, h = int(parts[1]), int(parts[2]); px = parts[4][-w*h*3:]
colourful = sum(1 for i in range(0, len(px), 3) if max(px[i:i+3]) - min(px[i:i+3]) > 80)
quant = {(px[i] >> 5, px[i+1] >> 5, px[i+2] >> 5) for i in range(0, len(px), 3)}
magenta = sum(1 for i in range(0, len(px), 3) if px[i] > 200 and px[i+1] < 60 and px[i+2] > 200)
n = w*h
print(f"shot {w}x{h}: colourful={colourful/n:.2f} distinct={len(quant)} magenta={magenta/n:.2f}")
sys.exit(0 if colourful/n > 0.15 and len(quant) >= 8 and magenta/n < 0.5 else 1)
PY
[ $? -eq 0 ] && ok "screenshot of the overlay region shows video pixels" || fail "screenshot does not look like video"

# ---- switching: next/prev + first-frame timing ----
C next >/dev/null; wait_state live 10 && expect_eq "next -> Loop B" "$(jst .camera)" "Loop B" || fail "next did not go live"
C prev >/dev/null; wait_state live 10 && expect_eq "prev -> Loop A" "$(jst .camera)" "Loop A" || fail "prev did not go live"
N0=$(first_frames | wc -l)
for i in $(seq 10); do
	if [ $((i % 2)) -eq 1 ]; then cam="Loop B"; else cam="Loop A"; fi
	C show "$cam" >/dev/null; wait_state live 10 || fail "switch to $cam did not reach live"
	sleep 0.3
done
first_frames | tail -n +$((N0 + 1)) | sort -n | awk '{a[NR]=$1} END{printf "info - first frame after 10 switches (ms): n=%d p50=%d max=%d min=%d\n", NR, a[int((NR+1)/2)], a[NR], a[1]}'
C hide >/dev/null
sleep 0.5

# ---- hidden: nothing held ----
expect_eq "hide removes the layer surface" "$(layer_ns)" 0
expect_eq "no RTSP connection while hidden" "$(established)" 0
FD1=$(pfd); TH1=$(pthr); RSS1=$(prss)
echo "info - baseline after first cycle: fds=$FD1 threads=$TH1 rss_kb=$RSS1 (startup: fds=$FD0 threads=$TH0)"

# ---- lifecycle cycles ----
for i in $(seq "$CYCLES"); do
	C toggle >/dev/null; wait_state live 10 || fail "cycle $i did not go live"
	C toggle >/dev/null; sleep 0.15
done
sleep 0.5
FD2=$(pfd); TH2=$(pthr); RSS2=$(prss)
echo "info - after $CYCLES show/hide cycles: fds=$FD2 threads=$TH2 rss_kb=$RSS2 (delta rss $((RSS2 - RSS1)) kB)"
expect_eq "fd count back to baseline after $CYCLES cycles" "$FD2" "$FD1"
[ "$TH2" -le "$TH1" ] && ok "thread count not above baseline ($TH2 <= $TH1)" || fail "threads grew: $TH1 -> $TH2"
[ $((RSS2 - RSS1)) -lt 30000 ] && ok "RSS growth over $CYCLES cycles < 30 MB" || fail "RSS grew $((RSS2 - RSS1)) kB"
expect_eq "no RTSP connection after cycles" "$(established)" 0
expect_eq "no layer after cycles" "$(layer_ns)" 0

# ---- failure paths ----
C show "Bad Password" >/dev/null; sleep 2.5
case "$(jst .state)" in reconnecting|error) ok "wrong password -> reconnecting/error";; *) fail "wrong password state: $(jst .state)";; esac
wait_state error 10; echo "info - bad password error text: $(jst .error)"
C hide >/dev/null
C show "Nothing Listens" >/dev/null; wait_state error 10 && ok "refused connection ends in error after retries" || fail "refused: state $(jst .state)"
echo "info - refused error text: $(jst .error)"
C hide >/dev/null
expect_eq "state hidden after failures" "$(jst .state)" hidden

# ---- secrets ----
leak=0
for f in "$T/out.log" "$T/err.log"; do
	grep -qF -e "$PW" -e "$PW_ENC" "$f" && leak=1
	grep -q 'rtsp://' "$f" && grep 'rtsp://' "$f" | grep -qv '\*\*\*:\*\*\*@' && leak=2
done
expect_eq "no password or raw rtsp URL in daemon stdout/stderr (debug level)" "$leak" 0
C show "Loop A" >/dev/null; wait_state live 10
l2=0
tr '\0' ' ' < "/proc/$DPID/cmdline" | grep -qF -e "$PW" -e "$PW_ENC" -e rtsp:// && l2=1
ps -eo args | grep -v -e rtsp_loopback -e 'grep -F' | grep -F -e "$PW" -e "$PW_ENC" >/dev/null && l2=2
C status --json | grep -qF -e "$PW" -e "$PW_ENC" -e rtsp:// && l2=3
expect_eq "no secret in cmdline, ps or status JSON while streaming" "$l2" 0
C hide >/dev/null

# ---- quit ----
C quit >/dev/null; sleep 0.5
kill -0 "$DPID" 2>/dev/null && fail "daemon still running after quit" || ok "daemon exits on quit"
DPID=""
echo
echo "$checks checks, $fails failed"
[ "$fails" -eq 0 ]
