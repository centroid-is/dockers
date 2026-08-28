#!/bin/bash
# One keyboard iteration against the test station: build keyboard.c in the
# kbforge container, hot-swap the binary into the running weston container,
# respawn the input method, screenshot both layouts, fetch the crops.
#
# The station runs the PR image, so the live binary is centroidx-keyboard.
# A hot-swap here is ephemeral (a container recreate restores the image's
# copy) — push the commit to the PR for anything durable.
set -e
S="$(cd "$(dirname "$0")" && pwd)"
KB=/Users/jonb/Projects/dockers/weston/keyboard/keyboard.c
H=centroid@10.50.10.11

scp -q "$KB" "$H":/tmp/keyboard.c
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
docker exec kbforge sh -c "ninja -C /src/weston/build clients/weston-keyboard 2>&1 | tail -1"
docker cp -q kbforge:/src/weston/build/clients/weston-keyboard /tmp/centroidx-keyboard
docker cp -q /tmp/centroidx-keyboard weston:/usr/local/bin/centroidx-keyboard.new
docker exec weston sh -c "chmod 755 /usr/local/bin/centroidx-keyboard.new && mv /usr/local/bin/centroidx-keyboard.new /usr/local/bin/centroidx-keyboard"

kill_match "editor-c[x]"
kill_match "centroidx-keyboar[d]"
sleep 2
if [ "$(count_match "centroidx-keyboar[d]")" = "0" ]; then
  echo "(weston did not respawn the keyboard; restarting container)"
  docker restart weston >/dev/null; sleep 10
fi

shoot() { # $1 = extra env, $2 = output name
  docker exec -u 1000 -d weston sh -c "export $E WESTON_EDITOR_AUTO_ACTIVATE=1 $1; exec /usr/local/bin/weston-editor-cx >/tmp/editor.log 2>&1"
  sleep 4
  docker exec -u 1000 weston sh -c "export $E; cd /tmp && weston-screenshooter >/dev/null 2>&1 && mv \$(ls -t wayland-screenshot*.png | head -1) $2"
  kill_match "editor-c[x]"
  sleep 1
}
shoot "" shot-text.png
shoot "WESTON_EDITOR_NUMERIC=1" shot-numeric.png
docker cp -q weston:/tmp/shot-text.png /tmp/shot-text.png
docker cp -q weston:/tmp/shot-numeric.png /tmp/shot-numeric.png
REMOTE
scp -q "$H":/tmp/shot-text.png "$H":/tmp/shot-numeric.png "$S/"
python3 - "$S" <<'EOF'
import sys
from PIL import Image
s = sys.argv[1]
# the panel is PANEL_WIDTH (900) x 200, bottom-centered on a 1920x1080
# output: x 510..1410, y 880..1080. Crop with a small margin around it.
Image.open(f"{s}/shot-text.png").crop((500, 860, 1420, 1080)).save(f"{s}/shot-text-crop.png")
Image.open(f"{s}/shot-numeric.png").crop((500, 860, 1420, 1080)).save(f"{s}/shot-numeric-crop.png")
EOF
echo DONE
