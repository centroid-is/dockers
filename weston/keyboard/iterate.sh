#!/bin/bash
# One keyboard iteration against the test station (KB_HOST=user@host): build keyboard.c in the
# kbforge container, hot-swap the binary into the running weston container,
# respawn the input method, screenshot every page, fetch the crops.
#
# The station runs the PR image, so the live binary is centroidx-keyboard.
# A hot-swap here is ephemeral (a container recreate restores the image's
# copy) — push the commit to the PR for anything durable.
#
# The pages other than the default one are reached with
# WESTON_KEYBOARD_START_STATE, and the languages with WESTON_KEYBOARD_START_LANG;
# the keyboard reads both on every activate.
# weston spawns the keyboard itself, so the value cannot be passed on a
# command line: the deploy installs the binary as centroidx-keyboard.real
# behind a wrapper that picks them up from /tmp/kbstate and /tmp/kblang, and
# enables all three languages when the station's weston does not set any.
set -e
S="$(cd "$(dirname "$0")" && pwd)"
KB="$S/keyboard.c"
H="${KB_HOST:?set KB_HOST to user@host of the test station}"

scp -q "$KB" "$H":/tmp/keyboard.c
scp -q "$S/keyboard-layouts.h" "$H":/tmp/keyboard-layouts.h
ssh -o BatchMode=yes "$H" 'bash -s' <<'REMOTE'
set -e
E="XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-1 HOME=/home/centroid"

# kill by scanning /proc — the image has no pkill/pgrep
kill_match() {
  docker exec -u 1000 weston sh -c "
    for p in /proc/[0-9]*; do
      tr '\0' ' ' < \$p/cmdline 2>/dev/null | grep -q '$1' && kill \${p#/proc/} 2>/dev/null
    done; true"
}
count_match() {
  docker exec -u 1000 weston sh -c "
    n=0; for p in /proc/[0-9]*; do
      tr '\0' ' ' < \$p/cmdline 2>/dev/null | grep -q '$1' && n=\$((n+1))
    done; echo \$n"
}

docker cp -q /tmp/keyboard.c kbforge:/src/weston/clients/keyboard.c
docker cp -q /tmp/keyboard-layouts.h kbforge:/src/weston/clients/keyboard-layouts.h
docker exec kbforge sh -c "ninja -C /src/weston/build clients/weston-keyboard 2>&1 | tail -1"
docker cp -q kbforge:/src/weston/build/clients/weston-keyboard /tmp/centroidx-keyboard
docker cp -q /tmp/centroidx-keyboard weston:/usr/local/bin/centroidx-keyboard.new
docker exec weston sh -c "chmod 755 /usr/local/bin/centroidx-keyboard.new"
docker exec weston sh -c "mv /usr/local/bin/centroidx-keyboard.new /usr/local/bin/centroidx-keyboard.real"

# write-then-rename: the running keyboard holds the old path open, so a
# plain redirect onto it is "Text file busy"
cat > /tmp/kbwrap <<'W'
#!/bin/sh
[ -r /tmp/kbstate ] && export WESTON_KEYBOARD_START_STATE="$(cat /tmp/kbstate)"
[ -r /tmp/kblang ] && export WESTON_KEYBOARD_START_LANG="$(cat /tmp/kblang)"
export KEYBOARD_LAYOUTS="${KEYBOARD_LAYOUTS:-is,en,pl}"
exec /usr/local/bin/centroidx-keyboard.real "$@"
W
docker cp -q /tmp/kbwrap weston:/usr/local/bin/centroidx-keyboard.wrap
docker exec weston sh -c "chmod 755 /usr/local/bin/centroidx-keyboard.wrap"
docker exec weston sh -c "mv /usr/local/bin/centroidx-keyboard.wrap /usr/local/bin/centroidx-keyboard"

respawn_keyboard() {
  kill_match "editor-c[x]"
  kill_match "centroidx-keyboar[d]"
  sleep 2
  if [ "$(count_match "centroidx-keyboar[d]")" = "0" ]; then
    echo "(weston did not respawn the keyboard; restarting container)"
    docker restart weston >/dev/null; sleep 10
  fi
}

shoot() { # $1 = start state (empty for letters), $2 = extra env, $3 = output, $4 = language
  docker exec -u 1000 weston sh -c "printf '%s' '$1' > /tmp/kbstate; printf '%s' '$4' > /tmp/kblang"
  respawn_keyboard
  docker exec -u 1000 -d weston sh -c "export $E WESTON_EDITOR_AUTO_ACTIVATE=1 $2; exec /usr/local/bin/weston-editor-cx >/tmp/editor.log 2>&1"
  sleep 4
  docker exec -u 1000 weston sh -c "export $E; cd /tmp && weston-screenshooter >/dev/null 2>&1 && mv \$(ls -t wayland-screenshot*.png | head -1) $3"
  kill_match "editor-c[x]"
  sleep 1
}
shoot "" "" shot-text-en.png en
shoot "" "" shot-text-is.png is
shoot "" "" shot-text-pl.png pl
shoot "symbols" "" shot-symbols.png
shoot "symbols2" "" shot-symbols2.png
shoot "" "WESTON_EDITOR_NUMERIC=1" shot-numeric.png
docker exec weston sh -c "rm -f /tmp/kbstate /tmp/kblang"
for f in text-en text-is text-pl symbols symbols2 numeric; do
  docker cp -q weston:/tmp/shot-$f.png /tmp/shot-$f.png
done
REMOTE
for f in text-en text-is text-pl symbols symbols2 numeric; do
  scp -q "$H":/tmp/shot-$f.png "$S/"
done
python3 - "$S" <<'EOF2'
import sys
from PIL import Image
s = sys.argv[1]
# the panel is PANEL_WIDTH (900) x 200, bottom-centered on a 1920x1080
# output: x 510..1410, y 880..1080. Crop with a small margin around it.
for name in ("text-en", "text-is", "text-pl", "symbols", "symbols2", "numeric"):
    Image.open(f"{s}/shot-{name}.png").crop((500, 860, 1420, 1080)).save(
        f"{s}/shot-{name}-crop.png")
EOF2
echo DONE
