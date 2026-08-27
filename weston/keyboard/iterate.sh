#!/bin/bash
# One keyboard iteration: build keyboard.c on the station, deploy into the
# weston container, restart the input method, screenshot text + numeric
# layouts, and fetch the crops back to the scratchpad.
set -e
S="$(cd "$(dirname "$0")" && pwd)"
KB=/Users/jonb/Projects/dockers/weston/keyboard/keyboard.c
H=centroid@10.50.10.11

scp -q "$KB" "$H":/tmp/keyboard.c
ssh -o BatchMode=yes "$H" '
set -e
docker cp -q /tmp/keyboard.c kbforge:/src/weston/clients/keyboard.c
docker exec kbforge sh -c "ninja -C /src/weston/build clients/weston-keyboard 2>&1 | tail -1"
docker cp -q kbforge:/src/weston/build/clients/weston-keyboard /tmp/weston-keyboard-cx
docker cp -q /tmp/weston-keyboard-cx weston:/usr/local/bin/weston-keyboard-cx.new
E="XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-1 HOME=/home/centroid"
docker exec weston sh -c "chmod 755 /usr/local/bin/weston-keyboard-cx.new && mv /usr/local/bin/weston-keyboard-cx.new /usr/local/bin/weston-keyboard-cx"
docker exec -u 1000 weston sh -c "pkill -f "[e]ditor-cx" || true; pkill -f "[k]eyboard-cx" || true"
sleep 2
# weston respawns the input method; if it did not, restart the container
if ! docker exec -u 1000 weston sh -c "pgrep -f "[k]eyboard-cx" >/dev/null"; then
  echo "(weston did not respawn keyboard; restarting container)"
  docker restart weston >/dev/null; sleep 8
fi
docker exec -u 1000 -d weston sh -c "export $E WESTON_EDITOR_AUTO_ACTIVATE=1; exec /usr/local/bin/weston-editor-cx >/tmp/editor.log 2>&1"
sleep 3
docker exec -u 1000 weston sh -c "export $E; cd /tmp && weston-screenshooter >/dev/null 2>&1 && mv \$(ls -t wayland-screenshot*.png | head -1) shot-text.png"
docker exec -u 1000 weston sh -c "pkill -f "[e]ditor-cx""; sleep 1
docker exec -u 1000 -d weston sh -c "export $E WESTON_EDITOR_AUTO_ACTIVATE=1 WESTON_EDITOR_NUMERIC=1; exec /usr/local/bin/weston-editor-cx >/tmp/editor.log 2>&1"
sleep 3
docker exec -u 1000 weston sh -c "export $E; cd /tmp && weston-screenshooter >/dev/null 2>&1 && mv \$(ls -t wayland-screenshot*.png | head -1) shot-numeric.png"
docker exec -u 1000 weston sh -c "pkill -f "[e]ditor-cx" || true"
docker cp -q weston:/tmp/shot-text.png /tmp/shot-text.png
docker cp -q weston:/tmp/shot-numeric.png /tmp/shot-numeric.png
'
scp -q "$H":/tmp/shot-text.png "$H":/tmp/shot-numeric.png "$S/"
python3 - "$S" <<'EOF'
import sys
from PIL import Image
s = sys.argv[1]
Image.open(f"{s}/shot-text.png").crop((520, 840, 1420, 1080)).save(f"{s}/shot-text-crop.png")
Image.open(f"{s}/shot-numeric.png").crop((520, 840, 1420, 1080)).save(f"{s}/shot-numeric-crop.png")
EOF
echo DONE
