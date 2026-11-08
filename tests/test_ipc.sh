#!/usr/bin/env bash
# End-to-end test: real daemon + real client over a Unix socket, stub video/overlay.
# Uses fake credentials only. Usage: tests/test_ipc.sh [path-to-cctv-overlay]
set -u

BIN=$(readlink -f "${1:-./cctv-overlay}")
command -v jq >/dev/null || { echo "SKIP/FAIL: jq is required for this test"; exit 1; }

PW='p@ss:w/rd%1'
PW_ENC='p%40ss%3Aw%2Frd%251'
T=$(mktemp -d "${TMPDIR:-/tmp}/cctv-ipc-XXXXXX")
export XDG_RUNTIME_DIR="$T/run"
mkdir -m 700 "$XDG_RUNTIME_DIR"
mkdir -p "$T/cfg"
unset CCTV_OVERLAY_PASSWORD CCTV_OVERLAY_USERNAME
export CCTV_OVERLAY_STUB_DELAY_MS=30
export CCTV_OVERLAY_BACKOFF_SCALE=0.01
SOCK="$XDG_RUNTIME_DIR/cctv-overlay/control.sock"
CFG="$T/cfg/config.ini"

cat > "$CFG" <<CFGEOF
[general]
log_level = debug
[recorder]
host = 192.168.1.100
username = tester
[camera.front]
name = Front Door
channel = 1
[camera.driveway]
name = Driveway
channel = 2
[camera.garden]
name = Garden
channel = 3
CFGEOF
printf '[recorder]\npassword = %s\n' "$PW" > "$T/cfg/secrets.ini"
chmod 600 "$T/cfg/secrets.ini"

fails=0 checks=0 DPID=""
cleanup() {
	[ -n "$DPID" ] && kill "$DPID" 2>/dev/null
	pkill -f "cctv-overlay daemon --config $CFG" 2>/dev/null
	rm -rf "$T"
}
trap cleanup EXIT

ok()   { checks=$((checks+1)); echo "ok   - $1"; }
fail() { checks=$((checks+1)); fails=$((fails+1)); echo "FAIL - $1"; }
expect_eq() { if [ "$2" = "$3" ]; then ok "$1"; else fail "$1 (got '$2', want '$3')"; fi; }

C() { "$BIN" --config "$CFG" "$@"; }

wait_for() { # wait_for <seconds> <cmd...>
	local n=$(( $1 * 50 )); shift
	while [ $n -gt 0 ]; do "$@" >/dev/null 2>&1 && return 0; sleep 0.02; n=$((n-1)); done
	return 1
}
jq_status() { C --no-spawn status --json | jq -r "$1"; }
sock_exists() { [ -S "$SOCK" ]; }
spawned_gone() { ! pgrep -f "[c]ctv-overlay daemon --config $CFG" >/dev/null; }
dpid_gone() { ! kill -0 "$DPID" 2>/dev/null; }
state_is() { [ "$(jq_status .state)" = "$1" ]; }

# ---------- exit codes without a daemon ----------
C --no-spawn status >/dev/null 2>&1; expect_eq "no daemon, --no-spawn -> exit 3" $? 3
"$BIN" frobnicate >/dev/null 2>&1; expect_eq "unknown command -> exit 2" $? 2
"$BIN" --bogus-flag >/dev/null 2>&1; expect_eq "unknown flag -> exit 2" $? 2
"$BIN" >/dev/null 2>&1; expect_eq "no command -> exit 2" $? 2
"$BIN" --config "$T/missing.ini" status >/dev/null 2>&1; expect_eq "missing config on spawn -> exit 2" $? 2
"$BIN" --config "$T/missing.ini" daemon >/dev/null 2>&1; expect_eq "daemon with missing config -> exit 2" $? 2
C --no-spawn quit >/dev/null 2>&1; expect_eq "quit with no daemon -> exit 3 (never spawns)" $? 3
sock_exists && fail "quit must not have spawned a daemon" || ok "quit did not spawn a daemon"

# ---------- start daemon ----------
"$BIN" daemon --config "$CFG" >"$T/out.log" 2>"$T/err.log" &
DPID=$!
wait_for 3 sock_exists; expect_eq "socket appears" $? 0
expect_eq "socket mode 0600" "$(stat -c %a "$SOCK")" 600
expect_eq "runtime dir mode 0700" "$(stat -c %a "$XDG_RUNTIME_DIR/cctv-overlay")" 700

C --no-spawn ping; expect_eq "ping -> exit 0" $? 0
expect_eq "version in reply" "$(C --no-spawn ping --json | jq -r .version)" "0.1.0-m2"
expect_eq "initial state hidden" "$(jq_status .state)" hidden
expect_eq "initial visible false" "$(jq_status .visible)" false
expect_eq "camera_count" "$(jq_status .camera_count)" 3
expect_eq "initial error null" "$(jq_status .error)" null

# second daemon refused
"$BIN" daemon --config "$CFG" >"$T/out2.log" 2>"$T/err2.log"; rc=$?
expect_eq "second daemon exits 1" $rc 1
grep -q "already running" "$T/err2.log" && ok "second daemon says 'already running'" || fail "second daemon message"
C --no-spawn ping; expect_eq "first daemon unaffected" $? 0

# ---------- toggle / next / prev / hide ----------
C --no-spawn toggle; expect_eq "toggle -> exit 0" $? 0
expect_eq "visible after toggle" "$(jq_status .visible)" true
wait_for 3 state_is live; expect_eq "reaches live (stub)" $? 0
expect_eq "camera is Front Door" "$(jq_status .camera)" "Front Door"

C --no-spawn next; expect_eq "next" "$(jq_status .camera_index)" 1
C --no-spawn next; expect_eq "next again" "$(jq_status .camera_index)" 2
C --no-spawn next; expect_eq "next wraps to 0" "$(jq_status .camera_index)" 0
C --no-spawn prev; expect_eq "prev wraps to 2" "$(jq_status .camera_index)" 2
expect_eq "prev camera name" "$(jq_status .camera)" Garden
wait_for 3 state_is live; expect_eq "live after switching" $? 0

C --no-spawn hide; expect_eq "hide -> exit 0" $? 0
expect_eq "hidden after hide" "$(jq_status .state)" hidden
expect_eq "visible false after hide" "$(jq_status .visible)" false

C --no-spawn next; expect_eq "next while hidden opens" "$(jq_status .visible)" true
expect_eq "next while hidden selects neighbour" "$(jq_status .camera)" "Front Door"
C --no-spawn hide
C --no-spawn show Driveway; expect_eq "show by name" "$(jq_status .camera_index)" 1
C --no-spawn show 2; expect_eq "show by index" "$(jq_status .camera)" Garden
C --no-spawn show front; expect_eq "show by id" "$(jq_status .camera)" "Front Door"
C --no-spawn show nosuchcam >/dev/null 2>"$T/e.txt"; expect_eq "show unknown -> exit 1" $? 1
grep -q "unknown camera" "$T/e.txt" && ok "unknown camera message" || fail "unknown camera message"

C --no-spawn toggle >/dev/null; expect_eq "toggle from visible hides" "$(jq_status .state)" hidden

# ---------- status/list formats ----------
C --no-spawn status | grep -q "^hidden:" && ok "human status line" || fail "human status line"
expect_eq "list --json count" "$(C --no-spawn list --json | jq '.cameras | length')" 3
expect_eq "list order" "$(C --no-spawn list --json | jq -r '[.cameras[].name] | join(",")')" "Front Door,Driveway,Garden"
C --no-spawn list | grep -q "Driveway" && ok "human list" || fail "human list"
C --no-spawn status --json | jq -e 'has("ok") and has("version") and has("visible") and has("state") and has("camera") and has("camera_index") and has("camera_count") and has("error")' >/dev/null \
	&& ok "status json has required fields" || fail "status json fields"

# ---------- reload ----------
C --no-spawn show driveway
C --no-spawn reload; expect_eq "reload -> exit 0" $? 0
expect_eq "reload keeps current camera" "$(jq_status .camera)" Driveway
cp "$CFG" "$CFG.bak"; printf '[overlay]\nwidth = 3\n' > "$CFG"
C --no-spawn reload >/dev/null 2>"$T/e.txt"; expect_eq "reload with bad config -> exit 1" $? 1
grep -q "width" "$T/e.txt" && ok "reload error is actionable" || fail "reload error message"
expect_eq "daemon still serving old config" "$(jq_status .camera)" Driveway
mv "$CFG.bak" "$CFG"
C --no-spawn hide

# ---------- protocol robustness (python is optional) ----------
if command -v python3 >/dev/null; then
	python3 -I - "$SOCK" <<'PYEOF'
import socket, sys, time, json
path = sys.argv[1]
def conn():
    s = socket.socket(socket.AF_UNIX); s.settimeout(5); s.connect(path); return s
def line(s):
    buf = b""
    while not buf.endswith(b"\n"):
        d = s.recv(4096)
        if not d: break
        buf += d
    return buf
bad = 0
s = conn(); s.sendall(b"not json\n"); r = json.loads(line(s)); bad += r["ok"] is not False
s = conn(); s.sendall(b'{"cmd":"ping"}\n'); r = json.loads(line(s)); bad += r["ok"] is not True
s = conn(); s.sendall(b"x" * 5000); r = json.loads(line(s)); bad += (r["ok"] is not False or "too large" not in r["error"])
t = time.time(); s = conn(); d = s.recv(10); dt = time.time() - t   # idle client is dropped
bad += not (d == b"" and 1.5 < dt < 3.5)
s = conn(); s.sendall(b'{"cmd":"ping"}\n'); r = json.loads(line(s)); bad += r["ok"] is not True  # still alive
sys.exit(bad)
PYEOF
	expect_eq "malformed/oversized/idle connections handled" $? 0
else
	echo "skip - python3 not found, protocol robustness checks skipped"
fi

# ---------- failure path (stub fails; daemon survives, ERROR reported) ----------
# (separate daemon would be needed to change env; covered by test_app. Check ERROR via missing password below.)

# ---------- no secret leakage while the daemon is up ----------
C --no-spawn toggle >/dev/null; sleep 0.2; C --no-spawn next >/dev/null
leak=0
for f in "$T/out.log" "$T/err.log" "$T/out2.log" "$T/err2.log"; do
	grep -qF "$PW" "$f" && leak=1; grep -qF "$PW_ENC" "$f" && leak=1
done
expect_eq "password absent from daemon stdout/stderr" $leak 0
[ -s "$T/err.log" ] && ok "daemon produced debug logs to scan ($(wc -l <"$T/err.log") lines)" || fail "no daemon logs captured"
ps_leak=0
ps -eo args | grep -F -e "$PW" -e "$PW_ENC" | grep -v grep >/dev/null && ps_leak=1
expect_eq "password absent from ps output" $ps_leak 0
tr '\0' ' ' < /proc/$DPID/cmdline | grep -qF "p@ss" && cl=1 || cl=0
expect_eq "password absent from /proc/PID/cmdline" $cl 0
C --no-spawn status --json | grep -qF "p@ss" && sl=1 || sl=0
expect_eq "password absent from status JSON" $sl 0
grep -q 'rtsp://' "$T/err.log" && fail "a URL appeared in the daemon log" || ok "no rtsp:// URL in daemon log"

# ---------- quit ----------
C --no-spawn quit; expect_eq "quit -> exit 0" $? 0
wait_for 3 dpid_gone; expect_eq "daemon process exits after quit" $? 0
wait "$DPID"; expect_eq "daemon exit status 0" $? 0; DPID=""
sock_exists && fail "socket removed on quit" || ok "socket removed on quit"
C --no-spawn status >/dev/null 2>&1; expect_eq "after quit -> exit 3" $? 3

# ---------- SIGTERM removes the socket ----------
"$BIN" daemon --config "$CFG" >"$T/out3.log" 2>"$T/err3.log" &
DPID=$!
wait_for 3 sock_exists; expect_eq "daemon restarts cleanly" $? 0
kill -TERM "$DPID"; wait "$DPID"; expect_eq "SIGTERM -> exit 0" $? 0; DPID=""
sock_exists && fail "socket removed on SIGTERM" || ok "socket removed on SIGTERM"

# ---------- stale socket from a crash is replaced ----------
"$BIN" daemon --config "$CFG" >/dev/null 2>&1 &
DPID=$!
wait_for 3 sock_exists
kill -KILL "$DPID"; wait "$DPID" 2>/dev/null; DPID=""
sock_exists && ok "SIGKILL leaves a stale socket (expected)" || fail "expected stale socket"
C status >/dev/null 2>&1; expect_eq "auto-spawn over stale socket works" $? 0
C --no-spawn quit >/dev/null; sleep 0.2

# ---------- auto-spawn ----------
sock_exists && fail "socket should be gone before auto-spawn test" || ok "clean slate"
C status --json >"$T/spawn.json" 2>"$T/spawn.err"; expect_eq "client auto-spawns daemon, exit 0" $? 0
expect_eq "auto-spawned daemon answers" "$(jq -r .state "$T/spawn.json")" hidden
expect_eq "second client reuses it" "$(C --no-spawn status --json | jq -r .camera_count)" 3
pgrep -f "cctv-overlay daemon --config $CFG" >/dev/null && ok "daemon argv has config path only" || fail "spawned daemon not found"
C --no-spawn quit >/dev/null
wait_for 3 spawned_gone; expect_eq "auto-spawned daemon quit" $? 0

# ---------- missing password: daemon works, stream start reports an error ----------
rm "$T/cfg/secrets.ini"
"$BIN" daemon --config "$CFG" >"$T/out4.log" 2>"$T/err4.log" &
DPID=$!
wait_for 3 sock_exists
expect_eq "daemon starts without password" "$(jq_status .state)" hidden
C --no-spawn list >/dev/null; expect_eq "list works without password" $? 0
C --no-spawn toggle >/dev/null
expect_eq "toggle without password -> error state" "$(jq_status .state)" error
jq_status .error | grep -qi password && ok "error mentions the missing password" || fail "error text"
C --no-spawn toggle >/dev/null   # ERROR + toggle retries (diagram)
C --no-spawn hide
expect_eq "hide from error -> hidden" "$(jq_status .state)" hidden
C --no-spawn quit >/dev/null; wait "$DPID" 2>/dev/null; DPID=""

# ---------- environment password also works (and never leaks) ----------
CCTV_OVERLAY_PASSWORD="$PW" "$BIN" daemon --config "$CFG" >"$T/out5.log" 2>"$T/err5.log" &
DPID=$!
wait_for 3 sock_exists
C --no-spawn toggle >/dev/null
wait_for 3 state_is live; expect_eq "env password reaches live" $? 0
C --no-spawn quit >/dev/null; wait "$DPID" 2>/dev/null; DPID=""
grep -qF "$PW" "$T/out5.log" "$T/err5.log" && fail "env password leaked into logs" || ok "env password not in logs"

echo
echo "$checks checks, $fails failed"
[ "$fails" -eq 0 ]
