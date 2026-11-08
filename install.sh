#!/usr/bin/env bash
# cctv-overlay installer: dependencies, build, user-level install, camera configuration, key bindings,
# password (stored only in a 0600 secrets file) and a live test.
#
#   ./install.sh                        interactive wizard (recommended for people)
#   ./install.sh --answers FILE --yes   unattended (for scripts / AI assistants; see docs/ai-install.md)
#   ./install.sh --set-password         set or change the recorder password (always typed by the user)
#   ./install.sh --check                check requirements only, change nothing
#   ./install.sh --test                 open the overlay once and report whether the cameras connect
#   ./install.sh --uninstall            remove the program and the managed key-binding block
#
# Safety rules this script follows:
#  - Never asks for, accepts or prints the password through arguments, environment or answer files.
#    The password is read from the terminal (no echo) and written straight into secrets.ini (mode 0600).
#  - Never overwrites an existing config.ini unless told to (it is backed up first).
#  - Edits only a marked block in your Hyprland bindings file, after a timestamped backup, and restores
#    the backup automatically if Hyprland reports new config errors.
#  - Installs into ~/.local (no root) unless PREFIX is set. Root is used only for `pacman -S`, after asking.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PREFIX=${PREFIX:-$HOME/.local}
BIN="$PREFIX/bin/cctv-overlay"
CONF_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/cctv-overlay"
CONF="$CONF_DIR/config.ini"
SECRETS="$CONF_DIR/secrets.ini"
HYPR_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/hypr"
MARK_BEGIN="cctv-overlay keybindings >>>"
MARK_END="<<< cctv-overlay keybindings"
TS=$(date +%Y%m%d-%H%M%S)

ARCH_PKGS=(base-devel pkgconf glib2 json-glib libepoxy gtk4 gtk4-layer-shell mpv jq python)

MODE=install
ANSWERS=""
ASSUME_YES=0
declare -A A=() # answers

# ---------------------------------------------------------------- output helpers
if [ -t 1 ]; then B=$'\e[1m' G=$'\e[32m' Y=$'\e[33m' R=$'\e[31m' N=$'\e[0m'; else B="" G="" Y="" R="" N=""; fi
say()  { printf '%s\n' "$*"; }
step() { printf '\n%s==> %s%s\n' "$B" "$*" "$N"; }
okay() { printf '  %s✓%s %s\n' "$G" "$N" "$*"; }
warn() { printf '  %s!%s %s\n' "$Y" "$N" "$*" >&2; }
die()  { printf '%sError:%s %s\n' "$R" "$N" "$*" >&2; exit 1; }

have_tty() { [ -r /dev/tty ] && [ -w /dev/tty ] && { : </dev/tty; } 2>/dev/null; }

usage() { sed -n '2,10p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

# ---------------------------------------------------------------- arguments
while [ $# -gt 0 ]; do
	case "$1" in
	--answers) ANSWERS=${2:-}; shift 2 || die "--answers needs a file" ;;
	--yes|-y) ASSUME_YES=1; shift ;;
	--set-password) MODE=password; shift ;;
	--check) MODE=check; shift ;;
	--test) MODE=test; shift ;;
	--uninstall) MODE=uninstall; shift ;;
	-h|--help) usage; exit 0 ;;
	*) die "unknown option '$1' (try --help)" ;;
	esac
done

# ---------------------------------------------------------------- answers file (never sourced)
if [ -n "$ANSWERS" ]; then
	[ -r "$ANSWERS" ] || die "cannot read answers file '$ANSWERS'"
	while IFS= read -r line || [ -n "$line" ]; do
		line=${line%$'\r'}
		[[ "$line" =~ ^[[:space:]]*(#|$) ]] && continue
		[[ "$line" =~ ^[[:space:]]*([A-Z0-9_]+)[[:space:]]*=[[:space:]]*(.*)$ ]] || die "answers file: cannot parse line: $line"
		key=${BASH_REMATCH[1]} val=${BASH_REMATCH[2]}
		val=${val%"${val##*[![:space:]]}"}
		[[ "$val" =~ ^\"(.*)\"$ ]] && val=${BASH_REMATCH[1]}
		case "$key" in
		*PASS*|*SECRET*|*TOKEN*)
			die "answers file must not contain a password ($key). Remove it; the user sets it with: ./install.sh --set-password" ;;
		esac
		A[$key]=$val
	done <"$ANSWERS"
fi
UNATTENDED=0
{ [ -n "$ANSWERS" ] || ! have_tty; } && UNATTENDED=1

# ask VAR[:ANSWER_KEY] "Question" "default"  -> answer from file, else prompt, else default
ask() {
	local var=${1%%:*} key=${1#*:} q=$2 def=${3:-} v
	if [ -n "${A[$key]+x}" ]; then printf -v "$var" '%s' "${A[$key]}"; return; fi
	if [ "$UNATTENDED" = 1 ]; then printf -v "$var" '%s' "$def"; return; fi
	if [ -n "$def" ]; then printf '%s [%s]: ' "$q" "$def" >/dev/tty; else printf '%s: ' "$q" >/dev/tty; fi
	IFS= read -r v </dev/tty || v=""
	printf -v "$var" '%s' "${v:-$def}"
}
# yesno "Question" default(y|n) [ANSWER_KEY] -> exit status
yesno() {
	local q=$1 def=$2 key=${3:-} v
	if [ -n "$key" ] && [ -n "${A[$key]+x}" ]; then v=${A[$key]}
	elif [ "$ASSUME_YES" = 1 ] || [ "$UNATTENDED" = 1 ]; then v=$def
	else
		printf '%s [%s]: ' "$q" "$([ "$def" = y ] && echo Y/n || echo y/N)" >/dev/tty
		IFS= read -r v </dev/tty || v=""
		v=${v:-$def}
	fi
	[[ "$v" =~ ^([yY]|[yY][eE][sS]|true|1)$ ]]
}
# choose VAR[:ANSWER_KEY] "Question" default option... (numbered menu; accepts number or value)
choose() {
	local var=${1%%:*} key=${1#*:} q=$2 def=$3; shift 3
	local opts=("$@") v i
	if [ -n "${A[$key]+x}" ]; then v=${A[$key]}
	elif [ "$UNATTENDED" = 1 ]; then v=$def
	else
		say "$q" >/dev/tty
		for i in "${!opts[@]}"; do printf '  %d) %s\n' $((i + 1)) "${opts[$i]}" >/dev/tty; done
		printf 'Choice [%s]: ' "$def" >/dev/tty
		IFS= read -r v </dev/tty || v=""
		v=${v:-$def}
	fi
	if [[ "$v" =~ ^[0-9]+$ ]] && [ "$v" -ge 1 ] && [ "$v" -le ${#opts[@]} ]; then v=${opts[$((v - 1))]}; fi
	v=${v%% *} # menu entries are "value  description"
	printf -v "$var" '%s' "$v"
}

ini_safe() { # a value that GKeyFile stores literally on one line
	[[ "$1" != *$'\n'* && "$1" != *$'\r'* ]] && [ -n "$1" ]
}

# ---------------------------------------------------------------- requirements
pkgcfg_min() { pkg-config --atleast-version="$2" "$1" 2>/dev/null; }

check_requirements() { # $1 = 1 to only report
	local missing=() bad=0
	step "Checking requirements"
	[ "$(uname -s)" = Linux ] && okay "Linux" || { warn "not Linux: unsupported"; bad=1; }
	if [ -n "${WAYLAND_DISPLAY:-}" ]; then okay "Wayland session ($WAYLAND_DISPLAY)"
	else warn "no Wayland session detected (WAYLAND_DISPLAY unset). The overlay needs a Wayland compositor with layer-shell."; fi
	if [ -n "${HYPRLAND_INSTANCE_SIGNATURE:-}" ] && command -v hyprctl >/dev/null; then
		okay "Hyprland $(hyprctl version -j 2>/dev/null | jq -r '.version // empty' 2>/dev/null) (tested: 0.56.2)"
	elif [ -n "${WAYLAND_DISPLAY:-}" ]; then
		warn "compositor is not Hyprland: it must support wlr-layer-shell (Sway, river, KDE Plasma 6 do; GNOME does not). Key bindings will not be set up automatically."
	fi
	if [ -d "$HOME/.local/share/omarchy" ] || command -v omarchy >/dev/null 2>&1; then okay "Omarchy detected"; fi

	if command -v pacman >/dev/null; then
		for p in "${ARCH_PKGS[@]}"; do
			if [ "$p" = base-devel ]; then # only the tools matter: a compiler and make
				{ command -v cc >/dev/null && command -v make >/dev/null; } || missing+=("$p")
			else pacman -Q "$p" >/dev/null 2>&1 || missing+=("$p"); fi
		done
	else
		warn "not an Arch-based system: install the equivalents of: ${ARCH_PKGS[*]} (see docs/requirements.md)"
		for t in cc make pkg-config jq python3; do command -v "$t" >/dev/null || { warn "$t is required"; bad=1; }; done
	fi
	if [ ${#missing[@]} -gt 0 ]; then
		warn "missing packages: ${missing[*]}"
		if [ "${1:-0}" = 1 ]; then bad=1
		elif yesno "Install them now with 'sudo pacman -S --needed ${missing[*]}'?" "$([ "$UNATTENDED" = 1 ] && echo n || echo y)" INSTALL_DEPS; then
			sudo pacman -S --needed --noconfirm "${missing[@]}" || die "package installation failed"
		else
			die "cannot build without: ${missing[*]}. Install them with: sudo pacman -S --needed ${missing[*]}"
		fi
	else
		okay "packages present"
	fi
	if command -v pkg-config >/dev/null; then
		local req=("gtk4 4.12" "gtk4-layer-shell-0 1.0" "glib-2.0 2.74" "mpv 2.0" "json-glib-1.0 1.6" "epoxy 1.5")
		local name ver
		for r in "${req[@]}"; do
			read -r name ver <<<"$r"
			if pkgcfg_min "$name" "$ver"; then okay "$name >= $ver ($(pkg-config --modversion "$name"))"
			else warn "$name >= $ver not found"; bad=1; fi
		done
	fi
	if command -v vainfo >/dev/null && vainfo 2>/dev/null | grep -q 'VAProfileH264'; then okay "VA-API hardware decode available"
	else warn "VA-API not confirmed (install libva-utils and run vainfo). Software decode will be used if needed."; fi
	return $bad
}

# ---------------------------------------------------------------- build + install
build_install() {
	step "Building"
	make -C "$HERE" -s clean >/dev/null
	make -C "$HERE" -s || die "build failed"
	okay "built $("$HERE/cctv-overlay" --version)"
	step "Installing to $PREFIX"
	# A running daemon keeps the old binary and config: stop it (it restarts on the next key press).
	[ -x "$BIN" ] && "$BIN" --no-spawn quit >/dev/null 2>&1 || true
	make -C "$HERE" -s PREFIX="$PREFIX" install >/dev/null || die "install failed"
	okay "$BIN"
	case ":$PATH:" in *":$PREFIX/bin:"*) ;; *) warn "$PREFIX/bin is not on your PATH (key bindings use the full path, so they still work)";; esac
}

# ---------------------------------------------------------------- camera system presets
# value | description | default port | path template ('' = ask) | channel numbering
SYSTEMS=(
	"dahua  Dahua / Amcrest / Lorex / other Dahua-based NVR or DVR"
	"hikvision  Hikvision / HiLook / Annke / other Hikvision-based NVR"
	"reolink  Reolink NVR"
	"uniview  Uniview (UNV) NVR"
	"standalone  Separate IP cameras (no recorder; each camera has its own address)"
	"custom  Other recorder: I know its RTSP path"
)
system_path() {
	case "$1" in
	dahua) echo "/cam/realmonitor" ;;
	hikvision) echo "/Streaming/Channels/{channel}0{stream}" ;;
	reolink) echo "/h264Preview_{channel02}_{mainsub}" ;;
	uniview) echo "/unicast/c{channel}/s{subtype}/live" ;;
	esac
}
CAM_BRANDS=(
	"dahua  Dahua / Amcrest camera"
	"hikvision  Hikvision camera"
	"reolink  Reolink camera"
	"tapo  TP-Link Tapo / VIGI camera"
	"uniview  Uniview camera"
	"custom  Other: I know the RTSP path"
)
camera_path() {
	case "$1" in
	dahua) echo "/cam/realmonitor?channel=1&subtype={subtype}" ;;
	hikvision) echo "/Streaming/Channels/10{stream}" ;;
	reolink) echo "/h264Preview_01_{mainsub}" ;;
	tapo) echo "/stream{stream}" ;;
	uniview) echo "/media/video{stream}" ;;
	esac
}

slug() { # camera id from a name: lowercase letters/digits only
	local s
	s=$(printf '%s' "$1" | tr '[:upper:]' '[:lower:]' | tr -cd 'a-z0-9')
	printf '%s' "${s:-cam}"
}

# ---------------------------------------------------------------- monitor
list_monitors() {
	command -v hyprctl >/dev/null && hyprctl monitors -j 2>/dev/null | jq -r '.[] | "\(.name)  \(.width)x\(.height) \(.description)"' 2>/dev/null || true
}

# ---------------------------------------------------------------- config
write_config() {
	step "Camera configuration"
	if [ -f "$CONF" ]; then
		okay "existing configuration found: $CONF"
		if ! yesno "Replace it with a new one (the current file is backed up)?" n OVERWRITE_CONFIG; then
			okay "keeping your existing configuration"
			return 0
		fi
	fi

	local SYSTEM HOST PORT USERNAME PATH_T STREAM NCAM MONITOR SIZE ANCHOR
	choose SYSTEM:SYSTEM "Which camera system do you have?" dahua "${SYSTEMS[@]}"
	case "$SYSTEM" in dahua|hikvision|reolink|uniview|standalone|custom) ;; *) die "unknown camera system '$SYSTEM'";; esac

	if [ "$SYSTEM" != standalone ]; then
		ask HOST:HOST "Recorder IP address or host name (e.g. 192.168.1.100)" ""
		[[ "$HOST" =~ ^[A-Za-z0-9.:_-]+$ ]] || die "invalid recorder address '$HOST'"
		ask PORT:PORT "Recorder RTSP port" 554
	fi
	[[ "${PORT:-554}" =~ ^[0-9]+$ ]] || die "invalid port '$PORT'"
	ask USERNAME:USERNAME "Recorder/camera user name (create a view-only user if you can)" admin
	ini_safe "$USERNAME" || die "invalid user name"
	if [ "$SYSTEM" = custom ]; then
		say "  Path template placeholders: {channel} {channel02} {subtype} (0 main/1 sub) {stream} (1 main/2 sub) {mainsub}" >&2
		ask PATH_T:PATH_TEMPLATE "RTSP path template (e.g. /live/ch{channel}_{subtype})" ""
		[[ "$PATH_T" == /* ]] || die "the path must start with /"
	else
		PATH_T=$(system_path "$SYSTEM")
	fi
	choose STREAM:STREAM "Which stream should single-camera view use?" main \
		"main  Main stream: sharp, usually starts fastest on NVRs (more bandwidth)" \
		"sub  Sub stream: low bandwidth/CPU, lower resolution"

	ask NCAM:CAMERAS "How many cameras do you have" 4
	[[ "$NCAM" =~ ^[0-9]+$ ]] && [ "$NCAM" -ge 1 ] && [ "$NCAM" -le 64 ] || die "number of cameras must be 1..64"

	local names=() ids=() chans=() hosts=() paths=() i n id c h pth brand last_brand=""
	for ((i = 1; i <= NCAM; i++)); do
		ask "n:CAMERA_${i}_NAME" "Name for camera $i (shown on the overlay)" "Camera $i"
		ini_safe "$n" || die "invalid camera name"
		for prev in "${names[@]}"; do [ "${prev,,}" != "${n,,}" ] || die "camera names must be unique ('$n')"; done
		id=$(slug "$n"); c=2
		while printf '%s\n' "${ids[@]}" | grep -qx "$id"; do id="$(slug "$n")$c"; c=$((c + 1)); done
		names+=("$n"); ids+=("$id")
		if [ "$SYSTEM" = standalone ]; then
			ask "h:CAMERA_${i}_HOST" "  IP address of '$n'" ""
			[[ "$h" =~ ^[A-Za-z0-9.:_-]+$ ]] || die "invalid address '$h'"
			choose "brand:CAMERA_${i}_BRAND" "  Make of '$n'?" "${last_brand:-dahua}" "${CAM_BRANDS[@]}"
			last_brand=$brand # default for the next camera
			if [ "$brand" = custom ]; then
				ask "pth:CAMERA_${i}_PATH" "  RTSP path of '$n' (e.g. /live/main)" ""
				[[ "$pth" == /* ]] || die "the path must start with /"
			else
				pth=$(camera_path "$brand")
			fi
			hosts+=("$h"); paths+=("$pth"); chans+=(1)
		else
			ask "c:CAMERA_${i}_CHANNEL" "  Recorder channel number of '$n'" "$i"
			[[ "$c" =~ ^[0-9]+$ ]] && [ "$c" -ge 1 ] || die "channel must be a number >= 1"
			chans+=("$c"); hosts+=(""); paths+=("")
		fi
	done

	local mons
	mons=$(list_monitors)
	if [ -n "$mons" ]; then
		[ "$UNATTENDED" = 1 ] || { say "Monitors:" >/dev/tty; printf '%s\n' "$mons" | sed 's/^/  /' >/dev/tty; }
		ask MONITOR:MONITOR "Monitor to show the overlay on (connector name, or 'focused')" "$(printf '%s\n' "$mons" | head -1 | cut -d' ' -f1)"
	else
		ask MONITOR:MONITOR "Monitor connector name (e.g. DP-1, HDMI-A-1), or 'focused'" focused
	fi
	[[ "$MONITOR" =~ ^[A-Za-z0-9_.-]+$ ]] || die "invalid monitor name"
	choose SIZE:SIZE "Overlay size?" 720x405 \
		"480x270  small" "600x338  medium" "720x405  large (recommended for the 4-camera grid)" "960x540  extra large"
	[[ "$SIZE" =~ ^[0-9]+x[0-9]+$ ]] || die "size must look like 720x405"
	choose ANCHOR:ANCHOR "Screen corner?" top-right "top-right" "top-left" "bottom-right" "bottom-left"

	mkdir -p "$CONF_DIR"; chmod 700 "$CONF_DIR"
	[ -f "$CONF" ] && cp -p "$CONF" "$CONF.bak.$TS" && okay "backup: $CONF.bak.$TS"
	local tmp
	tmp=$(mktemp "$CONF_DIR/.config.XXXXXX")
	{
		echo "# cctv-overlay configuration, written by install.sh on $(date -I)."
		echo "# No secrets here: the password lives in secrets.ini (mode 0600). Re-run ./install.sh to change"
		echo "# the setup, or edit this file and run: cctv-overlay reload. Comments must be on their own line."
		echo
		echo "[general]"
		echo "log_level = info"
		echo "monitor = $MONITOR"
		echo
		echo "[overlay]"
		echo "width = ${SIZE%x*}"
		echo "height = ${SIZE#*x}"
		echo "anchor = $ANCHOR"
		echo "margin_x = 24"
		echo "margin_y = 24"
		echo "layer = overlay"
		echo
		echo "[stream]"
		echo "transport = tcp"
		echo "# 0 = main stream, 1 = sub stream"
		echo "subtype = $([ "$STREAM" = sub ] && echo 1 || echo 0)"
		echo "hwdec = $(command -v vainfo >/dev/null && vainfo 2>/dev/null | grep -q VAProfileH264 && echo vaapi || echo auto-safe)"
		echo "reconnect = true"
		echo "# Audio starts off; toggle it with: cctv-overlay audio"
		echo "audio = false"
		echo
		echo "[recorder]"
		[ "$SYSTEM" != standalone ] && echo "host = $HOST" && echo "port = $PORT"
		echo "username = $USERNAME"
		[ "$SYSTEM" != standalone ] && echo "# camera system: $SYSTEM" && echo "path = $PATH_T"
		echo
		echo "# The grid (overview) view opens first on every toggle; next/prev then step through the cameras."
		echo "[grid]"
		echo "enabled = true"
		echo "# cameras = ${ids[0]}$([ "$NCAM" -gt 1 ] && printf ', %s' "${ids[@]:1:3}")"
		echo
		echo "# Cameras, in cycling order."
		for i in "${!names[@]}"; do
			echo "[camera.${ids[$i]}]"
			echo "name = ${names[$i]}"
			if [ -n "${hosts[$i]}" ]; then
				echo "host = ${hosts[$i]}"
				echo "path = ${paths[$i]}"
			else
				echo "channel = ${chans[$i]}"
			fi
			echo
		done
	} >"$tmp"
	chmod 600 "$tmp"
	mv "$tmp" "$CONF"
	okay "wrote $CONF ($NCAM cameras)"
}

# ---------------------------------------------------------------- password
set_password() {
	step "Recorder password"
	have_tty || die "no terminal available. The password must be typed by you, not passed in by a script or AI.
Run this yourself in a terminal:   $HERE/install.sh --set-password
(In Claude Code you can type:      ! $HERE/install.sh --set-password )"
	mkdir -p "$CONF_DIR"; chmod 700 "$CONF_DIR"
	local p1 p2 user=""
	printf 'It is stored only in %s (mode 0600) and never shown.\n' "$SECRETS" >/dev/tty
	while :; do
		printf 'Password: ' >/dev/tty; IFS= read -rs p1 </dev/tty; printf '\n' >/dev/tty
		printf 'Repeat:   ' >/dev/tty; IFS= read -rs p2 </dev/tty; printf '\n' >/dev/tty
		if [ -z "$p1" ]; then printf 'Empty password, try again.\n' >/dev/tty; continue; fi
		if [ "$p1" != "$p2" ]; then printf 'They do not match, try again.\n' >/dev/tty; continue; fi
		break
	done
	unset p2
	if [[ "$p1" =~ ^[[:space:]] || "$p1" =~ [[:space:]]$ ]]; then
		warn "the password starts or ends with a space; the config parser trims that. Set CCTV_OVERLAY_PASSWORD instead if login fails."
	fi
	# Keep an optional username line from an existing secrets file (never its password).
	[ -f "$SECRETS" ] && user=$(sed -n 's/^[[:space:]]*username[[:space:]]*=[[:space:]]*//p' "$SECRETS" | head -1)
	local tmp
	tmp=$(umask 077 && mktemp "$CONF_DIR/.secrets.XXXXXX")
	{
		printf '# Written by install.sh. Mode 0600. Never commit or share this file.\n'
		printf '[recorder]\n'
		[ -n "$user" ] && printf 'username = %s\n' "$user"
		printf 'password = %s\n' "$p1"
	} >"$tmp"
	unset p1
	chmod 600 "$tmp"
	mv -f "$tmp" "$SECRETS"
	okay "saved to $SECRETS (mode $(stat -c %a "$SECRETS"))"
	# A running daemon re-reads secrets on reload.
	"$BIN" --no-spawn reload >/dev/null 2>&1 || true
}

# ---------------------------------------------------------------- key bindings
# Formats: "omarchy" (o.bind in bindings.lua), "lua" (hl.bind in hyprland.lua), "conf-omarchy"
# (bindd in bindings.conf), "conf" (bind in hyprland.conf), "none".
detect_bind_target() {
	if [ -f "$HYPR_DIR/bindings.lua" ] && grep -q 'o\.bind' "$HYPR_DIR/bindings.lua"; then echo "omarchy $HYPR_DIR/bindings.lua"
	elif [ -f "$HYPR_DIR/hyprland.lua" ]; then echo "lua $HYPR_DIR/hyprland.lua"
	elif [ -f "$HYPR_DIR/bindings.conf" ]; then echo "conf-omarchy $HYPR_DIR/bindings.conf"
	elif [ -f "$HYPR_DIR/hyprland.conf" ]; then echo "conf $HYPR_DIR/hyprland.conf"
	else echo "none -"; fi
}

valid_keys() { [[ "$1" =~ ^([A-Z_]+[[:space:]]*\+[[:space:]]*)*[A-Za-z0-9_]+$ ]]; }

bind_line() { # fmt keys description command
	local fmt=$1 keys=$2 desc=$3 cmd=$4 mods key
	key=${keys##*+}; key=${key// /}
	mods=$([[ "$keys" == *+* ]] && printf '%s' "${keys%+*}" | tr -d ' ' | tr '+' ' ' || true)
	case "$fmt" in
	omarchy) printf 'o.bind("%s", "%s", "%s")\n' "$keys" "$desc" "$cmd" ;;
	lua) printf 'hl.bind("%s", hl.dsp.exec_cmd("%s"))\n' "$keys" "$cmd" ;;
	conf-omarchy) printf 'bindd = %s, %s, %s, exec, %s\n' "$mods" "$key" "$desc" "$cmd" ;;
	conf) printf 'bind = %s, %s, exec, %s\n' "$mods" "$key" "$cmd" ;;
	esac
}

configure_binds() {
	step "Key bindings"
	local target fmt file
	target=$(detect_bind_target); fmt=${target%% *}; file=${target#* }
	if [ "$fmt" = none ] || ! command -v hyprctl >/dev/null; then
		warn "no Hyprland configuration found; bind these commands in your compositor yourself:"
		say "    $BIN toggle | next | prev | audio"
		return 0
	fi
	okay "Hyprland config: $file ($fmt format)"
	say "  Key syntax: MODIFIERS + KEY, e.g. CTRL + KP_Multiply, SUPER + F9, KP_Divide. Type 'none' to skip one." >&2
	say "  Numpad keys: KP_Multiply (*), KP_Divide (/), KP_Subtract (-), KP_Add (+). A key without a modifier" >&2
	say "  is taken away from all other apps while Hyprland runs." >&2

	local K_TOGGLE K_NEXT K_PREV K_AUDIO
	ask K_TOGGLE:BIND_TOGGLE "Key to show/hide the overlay" "CTRL + KP_Multiply"
	ask K_NEXT:BIND_NEXT "Key for the next camera" "KP_Multiply"
	ask K_PREV:BIND_PREV "Key for the previous camera" "KP_Divide"
	ask K_AUDIO:BIND_AUDIO "Key to turn camera audio on/off" "CTRL + KP_Subtract"
	local c="#"; [[ "$fmt" == omarchy || "$fmt" == lua ]] && c="--"
	local block="$c >>> $MARK_BEGIN
$c Managed by cctv-overlay install.sh ($(date -I)). Re-run ./install.sh to change; delete this block to remove."
	local k desc verb
	for spec in "K_TOGGLE|Toggle CCTV overlay|toggle" "K_NEXT|Next CCTV view|next" "K_PREV|Previous CCTV view|prev" "K_AUDIO|Toggle CCTV audio|audio"; do
		IFS='|' read -r k desc verb <<<"$spec"
		k=${!k}
		[ "${k,,}" = none ] || [ -z "$k" ] && continue
		valid_keys "$k" || die "invalid key combination '$k'"
		block+=$'\n'"$(bind_line "$fmt" "$k" "$desc" "$BIN $verb")"
	done
	block+=$'\n'"$c $MARK_END <<<"

	if grep -q 'cctv-overlay' "$file" && ! grep -qF "$MARK_BEGIN" "$file"; then
		warn "$file already mentions cctv-overlay outside a managed block; check it for duplicate binds afterwards"
	fi
	local before after
	before=$(hyprctl configerrors 2>/dev/null || true)
	cp -p "$file" "$file.bak.$TS"
	okay "backup: $file.bak.$TS"
	python3 -I - "$file" "$MARK_BEGIN" "$MARK_END" "$block" <<'PY' || die "could not update $file"
import sys, re
path, begin, end, block = sys.argv[1:5]
text = open(path, encoding="utf-8").read()
pat = re.compile(r"^[^\n]*" + re.escape(begin) + r".*?" + re.escape(end) + r"[^\n]*\n?", re.S | re.M)
if pat.search(text):
    text = pat.sub(lambda m: block + "\n", text, count=1)
else:
    text = text.rstrip("\n") + "\n\n" + block + "\n"
open(path, "w", encoding="utf-8").write(text)
PY
	sleep 1 # Hyprland reloads on save
	hyprctl reload >/dev/null 2>&1 || true
	sleep 0.5
	after=$(hyprctl configerrors 2>/dev/null || true)
	if [ "$after" != "$before" ] && [ -n "$(printf '%s' "$after" | tr -d '[:space:]')" ]; then
		cp -p "$file.bak.$TS" "$file"; hyprctl reload >/dev/null 2>&1 || true
		die "Hyprland reported config errors after adding the binds; your file was restored. Errors:
$after"
	fi
	okay "key bindings active (hyprctl configerrors is clean)"
}

remove_binds() {
	local target fmt file
	target=$(detect_bind_target); fmt=${target%% *}; file=${target#* }
	[ "$fmt" = none ] && return 0
	grep -qF "$MARK_BEGIN" "$file" || return 0
	cp -p "$file" "$file.bak.$TS"
	python3 -I - "$file" "$MARK_BEGIN" "$MARK_END" <<'PY'
import sys, re
path, begin, end = sys.argv[1:4]
text = open(path, encoding="utf-8").read()
text = re.sub(r"\n?^[^\n]*" + re.escape(begin) + r".*?" + re.escape(end) + r"[^\n]*\n?", "\n", text, count=1, flags=re.S | re.M)
open(path, "w", encoding="utf-8").write(text)
PY
	okay "removed the managed block from $file (backup: $file.bak.$TS)"
	hyprctl reload >/dev/null 2>&1 || true
}

# ---------------------------------------------------------------- live test
live_test() {
	step "Live test"
	[ -n "${WAYLAND_DISPLAY:-}" ] || { warn "no Wayland session: skipping"; return 0; }
	[ -f "$SECRETS" ] || [ -n "${CCTV_OVERLAY_PASSWORD:-}" ] || { warn "no password set yet: run ./install.sh --set-password, then ./install.sh --test"; return 0; }
	"$BIN" --no-spawn quit >/dev/null 2>&1 || true
	sleep 0.3
	"$BIN" toggle >/dev/null || { warn "could not start the daemon; run '$BIN daemon' in a terminal to see why"; return 1; }
	local st="" i
	for i in $(seq 1 40); do
		st=$("$BIN" --no-spawn status --json 2>/dev/null) || true
		[ "$(jq -r '[.tiles[]?|select(.state=="connecting" or .state=="reconnecting")]|length' <<<"$st")" = 0 ] && [ $i -gt 4 ] && break
		sleep 0.5
	done
	jq -r '.tiles[]? | "  \(.state | if .=="live" then "✓" else "✗" end) \(.camera): \(.state)"' <<<"$st"
	local err
	err=$(jq -r '.error // empty' <<<"$st")
	[ -n "$err" ] && warn "last error: $err"
	sleep 2
	"$BIN" --no-spawn hide >/dev/null 2>&1 || true
	if [ "$(jq -r '[.tiles[]?|select(.state=="live")]|length' <<<"$st")" -gt 0 ]; then okay "cameras are reachable"
	else
		warn "no camera connected. Check the address, user name, password (./install.sh --set-password) and that RTSP is enabled on the recorder."
		warn "Details: $BIN daemon (in a terminal), or ~/.local/state/cctv-overlay/daemon.log"
		return 1
	fi
}

# ---------------------------------------------------------------- modes
case "$MODE" in
check)
	check_requirements 1 && { okay "all requirements met"; exit 0; } || exit 1 ;;
password)
	[ -x "$BIN" ] || BIN="$HERE/cctv-overlay"
	set_password
	exit 0 ;;
test)
	[ -x "$BIN" ] || die "not installed yet: run ./install.sh"
	live_test; exit $? ;;
uninstall)
	step "Uninstalling"
	[ -x "$BIN" ] && "$BIN" --no-spawn quit >/dev/null 2>&1 || true
	make -C "$HERE" -s PREFIX="$PREFIX" uninstall && okay "removed $BIN and its documentation"
	command -v hyprctl >/dev/null && remove_binds
	if yesno "Also delete your configuration and password ($CONF_DIR)?" n DELETE_CONFIG; then
		rm -rf "$CONF_DIR" && okay "deleted $CONF_DIR"
	else
		okay "kept $CONF_DIR"
	fi
	exit 0 ;;
esac

say "${B}cctv-overlay installer${N}  (Ctrl+C to stop at any time; nothing is changed before each step says so)"
check_requirements 0 || die "requirements not met (see above, and docs/requirements.md)"
build_install
write_config
# Validate offline (exit 1 only means "no password yet", which is handled below).
out=$("$BIN" --config "$CONF" check 2>&1) || [ $? -eq 1 ] || die "the configuration is not valid: $out"
okay "configuration is valid"
configure_binds

if [ -f "$SECRETS" ] && grep -q '^[[:space:]]*password[[:space:]]*=' "$SECRETS"; then
	okay "a password is already stored in $SECRETS"
	if [ "$UNATTENDED" = 0 ] && yesno "Change it now?" n; then set_password; fi
elif [ "$UNATTENDED" = 0 ]; then
	set_password
else
	step "Recorder password"
	warn "not set. The user must type it themselves (never through an AI chat or a file):"
	say "    $HERE/install.sh --set-password"
	say "    (in Claude Code: ! $HERE/install.sh --set-password)   then test with: $HERE/install.sh --test"
fi

if [ -f "$SECRETS" ] && yesno "Open the overlay now to test the cameras?" y RUN_TEST; then live_test || true; fi

step "Done"
say "  Toggle: your toggle key (or: $BIN toggle). It opens on the 4-camera grid; next/prev step through cameras."
say "  Audio:  your audio key (or: $BIN audio). A speaker icon shows while audio is on."
say "  Config: $CONF   (after editing: $BIN reload)"
say "  Logs:   ~/.local/state/cctv-overlay/daemon.log"
