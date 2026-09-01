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
# This is installed as /usr/local/bin/weston, ahead of /usr/bin/weston on PATH,
# so stations keep working with the `exec weston -c ...` entrypoint they
# already have in docker-compose.yml — no per-station edit needed.
#
# Set WESTON_MIRROR_OF to override the auto-detected connector.
set -eu

log() { echo "centroidx-weston: $*" >&2; }

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

resolved="$config"
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

	resolved="${XDG_RUNTIME_DIR:-/tmp}/weston.ini"
	if ! : > "$resolved" 2>/dev/null; then
		resolved="/tmp/weston.ini"
		: > "$resolved"
	fi

	if [ -n "$connector" ]; then
		log "mirroring DRM output $connector to the VNC output"
		sed "s/@MIRROR_OF@/$connector/" "$config" > "$resolved"
	else
		log "no connected DRM output found; VNC gets a standalone output"
		sed '/@MIRROR_OF@/d' "$config" > "$resolved"
	fi
fi

exec /usr/bin/weston -c "$resolved" ${tls_flag:+"$tls_flag"} "$@"
