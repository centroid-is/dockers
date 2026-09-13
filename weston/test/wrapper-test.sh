#!/bin/sh
# Tests for weston-wrapper.sh's config resolution, run without weston: the
# wrapper's WESTON_WRAPPER_DRY_RUN prints the resolved weston.ini and the
# environment it would start weston with. Each case gets a fresh fake host
# /etc/default and runtime dir.
#
#   sh weston/test/wrapper-test.sh
set -eu

here=$(cd "$(dirname "$0")" && pwd)
wrapper="$here/../weston-wrapper.sh"
ini="$here/../weston.ini"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

failures=0
cases=0

# run NAME [VAR=value ...] -- sets up a clean environment and runs the wrapper
# with the given variables; the host files come from $locale and $keyboard.
run() {
	name="$1"
	shift
	cases=$((cases + 1))
	rm -rf "$work/host" "$work/run"
	mkdir -p "$work/host" "$work/run"
	[ -z "${locale-}" ] || printf '%s\n' "$locale" > "$work/host/locale"
	[ -z "${keyboard-}" ] || printf '%s\n' "$keyboard" > "$work/host/keyboard"
	if ! out=$(env -u KEYBOARD_LAYOUTS -u KEYBOARD_DEFAULT -u KEYBOARD_OPTIONS \
	           -u WESTON_MIRROR_OF \
	           WESTON_WRAPPER_DRY_RUN=1 \
	           WESTON_HOST_ETC_DEFAULT="$work/host" \
	           XDG_RUNTIME_DIR="$work/run" \
	           "$@" sh "$wrapper" -c "$ini" 2>"$work/stderr"); then
		echo "FAIL $name: wrapper exited non-zero"
		sed 's/^/    /' "$work/stderr"
		failures=$((failures + 1))
		out=""
	fi
}

# expect LINE -- the dry-run output must contain LINE exactly.
expect() {
	if ! printf '%s\n' "$out" | grep -qxF -- "$1"; then
		echo "FAIL $name: expected line '$1'"
		printf '%s\n' "$out" | grep -E '^(KEYBOARD_|keymap_|mirror-of|exec )' | sed 's/^/    got: /'
		failures=$((failures + 1))
	fi
}

# reject PATTERN -- no output line may match the extended regex PATTERN.
reject() {
	if printf '%s\n' "$out" | grep -qE -- "$1"; then
		echo "FAIL $name: unexpected match for '$1'"
		failures=$((failures + 1))
	fi
}

# expect_log TEXT -- stderr must mention TEXT.
expect_log() {
	if ! grep -qF -- "$1" "$work/stderr"; then
		echo "FAIL $name: expected log containing '$1'"
		sed 's/^/    log: /' "$work/stderr"
		failures=$((failures + 1))
	fi
}

locale="" keyboard=""
run "nothing set, no host files"
expect "KEYBOARD_LAYOUTS=is,en,pl"
expect "keymap_layout=is,us,pl"
expect "keymap_options=grp:alt_shift_toggle"
expect_log "first of the enabled layouts"
reject '@[A-Z_]+@'

locale='LANG="pl_PL.UTF-8"' keyboard=""
run "host locale picks the default"
expect "KEYBOARD_LAYOUTS=pl,is,en"
expect "keymap_layout=pl,is,us"
expect_log "host locale pl_PL.UTF-8"

locale='LANG="en_US.UTF-8"
LANGUAGE="en_US:en"' keyboard='XKBLAYOUT="is"'
run "host locale outranks host XKBLAYOUT"
expect "KEYBOARD_LAYOUTS=en,is,pl"
expect "keymap_layout=us,is,pl"

locale='LANG=de_DE.UTF-8' keyboard='XKBMODEL="pc105"
XKBLAYOUT="is"'
run "unsupported locale falls through to XKBLAYOUT"
expect "KEYBOARD_LAYOUTS=is,en,pl"
expect_log "host XKBLAYOUT is"

locale='LANG=C.UTF-8' keyboard='XKBLAYOUT="us,is"'
run "C locale and a multi-layout XKBLAYOUT"
expect "KEYBOARD_LAYOUTS=en,is,pl"

locale='LANG=en_US.UTF-8
LC_ALL=is_IS.UTF-8' keyboard=""
run "LC_ALL outranks LANG"
expect "KEYBOARD_LAYOUTS=is,en,pl"

locale='LANG="is_IS.UTF-8"' keyboard='XKBLAYOUT="is"'
run "KEYBOARD_DEFAULT overrides the host" KEYBOARD_DEFAULT=pl
expect "KEYBOARD_LAYOUTS=pl,is,en"
expect_log "from KEYBOARD_DEFAULT"

locale='LANG="en_US.UTF-8"' keyboard=""
run "a single enabled layout ignores a host that names another" KEYBOARD_LAYOUTS=is
expect "KEYBOARD_LAYOUTS=is"
expect "keymap_layout=is"

locale="" keyboard=""
run "us alias, case, spaces, duplicates and junk" "KEYBOARD_LAYOUTS=PL, us,xx,pl"
expect "KEYBOARD_LAYOUTS=pl,en"
expect "keymap_layout=pl,us"
expect_log "ignoring unknown keyboard layout 'xx'"

locale="" keyboard=""
run "only junk falls back to all three" KEYBOARD_LAYOUTS=de,fr
expect "KEYBOARD_LAYOUTS=is,en,pl"

locale="" keyboard=""
run "a default outside the list is enabled" KEYBOARD_LAYOUTS=is,en KEYBOARD_DEFAULT=PL
expect "KEYBOARD_LAYOUTS=pl,is,en"
expect_log "enabling it"

locale='LANG=pl_PL.UTF-8' keyboard=""
run "an unknown default falls through to the host" KEYBOARD_DEFAULT=klingon
expect "KEYBOARD_LAYOUTS=pl,is,en"
expect_log "ignoring unknown KEYBOARD_DEFAULT"

locale="" keyboard=""
run "custom options" KEYBOARD_OPTIONS=grp:ctrl_shift_toggle,compose:ralt
expect "keymap_options=grp:ctrl_shift_toggle,compose:ralt"

locale="" keyboard=""
run "empty options drop the line" KEYBOARD_OPTIONS=
reject '^keymap_options'
expect "keymap_layout=is,us,pl"

locale="" keyboard=""
run "options that would break sed are refused" "KEYBOARD_OPTIONS=grp:x/y&z"
expect "keymap_options=grp:alt_shift_toggle"

locale="" keyboard=""
run "an explicit connector is mirrored" WESTON_MIRROR_OF=HDMI-A-1
expect "mirror-of=HDMI-A-1"

locale="" keyboard=""
run "tls stays disabled without a certificate"
expect "exec /usr/bin/weston -c $work/run/weston.ini --disable-transport-layer-security"

locale="" keyboard=""
run "missing host directory" WESTON_HOST_ETC_DEFAULT=/nonexistent
expect "KEYBOARD_LAYOUTS=is,en,pl"

if [ "$failures" -ne 0 ]; then
	echo "$failures failure(s) in $cases cases"
	exit 1
fi
echo "all $cases cases passed"
