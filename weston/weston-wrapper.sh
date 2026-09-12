#!/bin/sh
# Fill in the parts of weston.ini and the weston command line that only the
# running container knows, then hand over to the real weston.
#
# Weston 16 removed screen-share.so. Sharing the panel over VNC is now done by
# running the VNC backend next to DRM in one compositor and giving the VNC
# output a `mirror-of=<connector>` key, in place of the nested
# `weston --backend=vnc-backend.so --shell=fullscreen-shell.so` child process
# the module used to spawn. Two things do not survive that move into a static
# config file:
#
#   * mirror-of takes the literal DRM connector name, which differs from
#     station to station (eDP-1 on one panel, HDMI-A-1 on the next), so
#     weston.ini ships a @MIRROR_OF@ placeholder for this script to resolve.
#
#   * --disable-transport-layer-security has no weston.ini equivalent, and the
#     nested child used to get it on its command line. Without it the VNC
#     backend refuses to start unless TLS credentials are configured, and with
#     TLS on, neatvnc stops offering RA2ne — the only security type a browser
#     can use to verify the server. So it is added here unless weston.ini
#     configures a certificate after all.
#
# The keyboard layouts are the third thing. weston.ini's [keyboard] ships
# @KEYMAP_LAYOUT@ and @KEYMAP_OPTIONS@, resolved from the environment the
# compose file gives this container (see resolve_keyboard below and
# README.md). The normalised list is exported as KEYBOARD_LAYOUTS too: weston
# starts the on-screen keyboard with its own environment, so that is how the
# input method learns which languages to offer and which one comes first.
#
# This is installed as /usr/local/bin/weston, ahead of /usr/bin/weston on PATH,
# so stations keep working with the `exec weston -c ...` entrypoint they
# already have in docker-compose.yml — no per-station edit needed.
#
# Set WESTON_MIRROR_OF to override the auto-detected connector.
# Set WESTON_WRAPPER_DRY_RUN=1 to print the resolved config and environment
# instead of starting weston; the tests and station probes use it.
set -eu

log() { echo "centroidx-weston: $*" >&2; }

dry_run="${WESTON_WRAPPER_DRY_RUN-}"

# Pull the config path out of the arguments; everything else is rotated to the
# back of "$@" untouched and passed through.
config=""
n=$#
while [ "$n" -gt 0 ]; do
	case "$1" in
	-c|--config)  config="${2-}"
	              if [ $# -gt 1 ]; then shift 2; n=$((n - 2)); else shift; n=$((n - 1)); fi ;;
	-c*)          config="${1#-c}"; shift; n=$((n - 1)) ;;
	--config=*)   config="${1#--config=}"; shift; n=$((n - 1)) ;;
	*)            set -- "$@" "$1"; shift; n=$((n - 1)) ;;
	esac
done
: "${config:=/home/centroid/.config/weston.ini}"

# An unreadable config is weston's error to report, not ours.
if [ ! -r "$config" ]; then
	if [ "$dry_run" = 1 ]; then
		log "config $config is not readable"
		exit 1
	fi
	exec /usr/bin/weston -c "$config" "$@"
fi

# Certificate configured, or the caller already decided: leave TLS alone.
tls_flag="--disable-transport-layer-security"
if grep -q '^tls-cert=' "$config"; then
	tls_flag=""
fi
for arg in "$@"; do
	case "$arg" in
	--disable-transport-layer-security|--vnc-tls-cert|--vnc-tls-cert=*) tls_flag="" ;;
	esac
done

# --- keyboard layouts --------------------------------------------------------
#
# KEYBOARD_LAYOUTS  comma list of is, en, pl (us is accepted for en); unset or
#                   empty means all three.
# KEYBOARD_DEFAULT  the layout keyboards start in. Unset or empty means: the
#                   host's LANG (LC_ALL wins if set), else the host's
#                   XKBLAYOUT, else the first of KEYBOARD_LAYOUTS.
# KEYBOARD_OPTIONS  xkb options; grp:alt_shift_toggle unless set. Set it empty
#                   to have no switch shortcut at all.
#
# The host's files are read from WESTON_HOST_ETC_DEFAULT, which the compose
# file fills by mounting the host's /etc/default read-only. The directory, not
# the two files: Docker creates a missing bind source as a directory, and a
# directory standing where the host's /etc/default/locale should be breaks
# pam_env on the host.

# Print the supported layout code a token names, or nothing.
layout_code() {
	case "$(printf '%s' "$1" | tr '[:upper:]' '[:lower:]')" in
	is)    echo is ;;
	en|us) echo en ;;
	pl)    echo pl ;;
	esac
}

# The xkb symbols file a layout code is spelled as.
xkb_layout() {
	case "$1" in
	en) echo us ;;
	*)  echo "$1" ;;
	esac
}

# The last KEY=value assignment in a shell-style file, quotes stripped.
file_value() {
	[ -r "$1" ] || return 0
	sed -n "s/^[[:space:]]*$2=//p" "$1" | tail -n 1 | tr -d "\"'"
}

resolve_keyboard() {
	host_default="${WESTON_HOST_ETC_DEFAULT:-/run/host/etc/default}"

	layouts=""
	for token in $(printf '%s' "${KEYBOARD_LAYOUTS-}" | tr ',' ' '); do
		code=$(layout_code "$token")
		if [ -z "$code" ]; then
			log "ignoring unknown keyboard layout '$token'"
			continue
		fi
		case " $layouts " in
		*" $code "*) ;;
		*) layouts="${layouts:+$layouts }$code" ;;
		esac
	done
	[ -n "$layouts" ] || layouts="is en pl"

	default=""
	source=""

	if [ -n "${KEYBOARD_DEFAULT-}" ]; then
		code=$(layout_code "$KEYBOARD_DEFAULT")
		if [ -z "$code" ]; then
			log "ignoring unknown KEYBOARD_DEFAULT '$KEYBOARD_DEFAULT'"
		else
			default="$code"
			source="KEYBOARD_DEFAULT"
			case " $layouts " in
			*" $code "*) ;;
			*) log "KEYBOARD_DEFAULT '$code' is not in KEYBOARD_LAYOUTS; enabling it"
			   layouts="$code $layouts" ;;
			esac
		fi
	fi

	if [ -z "$default" ]; then
		locale=$(file_value "$host_default/locale" LC_ALL)
		[ -n "$locale" ] || locale=$(file_value "$host_default/locale" LANG)
		if [ -n "$locale" ]; then
			# en_US.UTF-8 -> en; C and POSIX name no language.
			code=$(layout_code "${locale%%[_.@]*}")
			case " $layouts " in
			*" ${code:-none} "*) default="$code"; source="host locale $locale" ;;
			esac
		fi
	fi

	if [ -z "$default" ]; then
		xkb=$(file_value "$host_default/keyboard" XKBLAYOUT)
		if [ -n "$xkb" ]; then
			code=$(layout_code "${xkb%%,*}")
			case " $layouts " in
			*" ${code:-none} "*) default="$code"; source="host XKBLAYOUT $xkb" ;;
			esac
		fi
	fi

	if [ -z "$default" ]; then
		default="${layouts%% *}"
		source="first of the enabled layouts"
	fi

	ordered="$default"
	keymap_layout=$(xkb_layout "$default")
	for code in $layouts; do
		[ "$code" = "$default" ] && continue
		ordered="$ordered,$code"
		keymap_layout="$keymap_layout,$(xkb_layout "$code")"
	done

	# Options go into weston.ini through sed, so hold them to what xkb option
	# names are made of rather than escaping whatever arrives.
	keymap_options="${KEYBOARD_OPTIONS-grp:alt_shift_toggle}"
	case "$keymap_options" in
	*[!A-Za-z0-9_:,.+-]*)
		log "ignoring KEYBOARD_OPTIONS '$keymap_options'; using grp:alt_shift_toggle"
		keymap_options="grp:alt_shift_toggle" ;;
	esac

	KEYBOARD_LAYOUTS="$ordered"
	export KEYBOARD_LAYOUTS
	log "keyboard layouts $ordered (default $default from $source)"
}

resolve_keyboard

# --- resolve the placeholders ------------------------------------------------

resolved="$config"
if grep -q '@[A-Z_][A-Z_]*@' "$config"; then
	mirror_expr='s/@MIRROR_OF@/@MIRROR_OF@/'
	if grep -q '@MIRROR_OF@' "$config"; then
		connector="${WESTON_MIRROR_OF-}"
		if [ -z "$connector" ]; then
			for status in /sys/class/drm/card*-*/status; do
				[ -r "$status" ] || continue
				[ "$(cat "$status")" = "connected" ] || continue
				name="${status%/status}"
				name="${name##*/}"
				connector="${name#card*-}"
				break
			done
		fi

		if [ -n "$connector" ]; then
			log "mirroring DRM output $connector to the VNC output"
			mirror_expr="s/@MIRROR_OF@/$connector/"
		else
			log "no connected DRM output found; VNC gets a standalone output"
			mirror_expr='/@MIRROR_OF@/d'
		fi
	fi

	if [ -n "$keymap_options" ]; then
		options_expr="s/@KEYMAP_OPTIONS@/$keymap_options/"
	else
		options_expr='/@KEYMAP_OPTIONS@/d'
	fi

	resolved="${XDG_RUNTIME_DIR:-/tmp}/weston.ini"
	if ! : > "$resolved" 2>/dev/null; then
		resolved="/tmp/weston.ini"
		: > "$resolved"
	fi

	sed -e "$mirror_expr" \
	    -e "s/@KEYMAP_LAYOUT@/$keymap_layout/" \
	    -e "$options_expr" \
	    "$config" > "$resolved"
fi

if [ "$dry_run" = 1 ]; then
	echo "KEYBOARD_LAYOUTS=$KEYBOARD_LAYOUTS"
	echo "exec /usr/bin/weston -c $resolved${tls_flag:+ $tls_flag}${*:+ $*}"
	cat "$resolved"
	exit 0
fi

exec /usr/bin/weston -c "$resolved" ${tls_flag:+"$tls_flag"} "$@"
