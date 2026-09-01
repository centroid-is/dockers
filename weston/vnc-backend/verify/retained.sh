#!/bin/sh
# Regression checks for the selection this backend keeps serving after the
# client that published it has gone, and for a requester that walks away
# mid-transfer.
#
# Both assert byte COUNTS, not just that text appears: the failure modes here
# read correctly at a glance and are only visible when counted.
#
#   retained.sh [image]      default image: weston-clip-test
#
# Needs a `vnctest` docker network and an image carrying wl-clipboard and the
# compiled abort-reader; see README.md in this directory.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
IMAGE=${1:-weston-clip-test}

SHORT_HEX=6672612d726967676e756d2dc3bec3b0c3a12d32303236   # 23 bytes, UTF-8
SHORT_LEN=23
BIG_LEN=4194304
# Large enough that the compositor is still writing long after the reader has
# read its fill, and under neatvnc's 10 MB MAX_CUT_TEXT_SIZE.
ABORT_LEN=9000000

WEX="docker exec -e XDG_RUNTIME_DIR=/tmp/xdg -e WAYLAND_DISPLAY=wayland-1 --user centroid westonc"
fail=0

note()  { echo "  $1"; }
check() {
  if [ "$2" = "$3" ]; then
    echo "  ok   $1: $2"
  else
    echo "  FAIL $1: got $2, wanted $3"
    fail=1
  fi
}

start_weston() {
  docker rm -f westonc >/dev/null 2>&1 || true
  docker run -d --name westonc --network vnctest --user root "$IMAGE" sh -c \
    "echo 'centroid:foo' | chpasswd && mkdir -p /tmp/xdg && chown centroid /tmp/xdg \
     && chmod 700 /tmp/xdg && exec su -s /bin/bash -c \
     'XDG_RUNTIME_DIR=/tmp/xdg exec weston --logger-scopes=log,vnc-backend \
      -c /home/centroid/.config/weston.ini -B headless,vnc --fake-seat \
      --width=1280 --height=800' centroid" >/dev/null
  sleep 6
}

start_client() {
  docker rm -f vncclient >/dev/null 2>&1 || true
  docker run -d --name vncclient --network vnctest -v "$HERE:/w" python:3.12-slim \
    sh -c "pip -q install pycryptodome \
           && python -u /w/clipboard.py westonc 5900 centroid foo 120 $1" >/dev/null
  i=0
  while [ $i -lt 90 ]; do
    docker logs vncclient 2>&1 | grep -q "^connected:" && break
    sleep 1; i=$((i + 1))
  done
  sleep 3
}

weston_alive() {
  docker inspect westonc --format '{{.State.Status}}' 2>/dev/null || echo gone
}

paste_len() { $WEX sh -c 'wl-paste -n 2>/dev/null | wc -c' 2>/dev/null | tr -d ' \r' || echo "?"; }

echo "== a short selection survives its publisher, at the right length =="
start_weston
start_client $SHORT_HEX
check "while the client is connected" "$(paste_len)" "$SHORT_LEN"
check "pasted twice"                  "$(paste_len)" "$SHORT_LEN"
docker rm -f vncclient >/dev/null 2>&1 || true
sleep 3
check "after the client disconnected" "$(paste_len)" "$SHORT_LEN"
check "and again"                     "$(paste_len)" "$SHORT_LEN"
check "and again"                     "$(paste_len)" "$SHORT_LEN"

echo "== a large one too, where the transfer needs several writable events =="
start_client @$BIG_LEN
check "while the client is connected" "$(paste_len)" "$BIG_LEN"
docker rm -f vncclient >/dev/null 2>&1 || true
sleep 3
check "after the client disconnected" "$(paste_len)" "$BIG_LEN"

echo "== a reader that closes mid-transfer must not take weston with it =="
# wl-paste is no use here: it drains the whole offer into memory as fast as it
# arrives, so killing it never strands a write, and an unguarded build sails
# through. abort-reader reads continuously and then drops the pipe part-way,
# which is the case that raises SIGPIPE on the compositor's next write.
start_client @$ABORT_LEN
round=0
while [ $round -lt 8 ]; do
  round=$((round + 1))
  $WEX abort-reader 262144 >/dev/null 2>&1 || true
  [ "$(weston_alive)" = "running" ] || break
done
check "weston after $round readers closed mid-transfer" "$(weston_alive)" "running"

if [ "$(weston_alive)" = "running" ]; then
  sleep 2
  check "and the selection still reads back whole" "$(paste_len)" "$ABORT_LEN"
fi
docker rm -f vncclient >/dev/null 2>&1 || true

echo "== the client-facing direction, repeated, must not accumulate =="
start_weston
start_client ""
COPY_TEXT='fra-riggnum-þðá-2026'
n=0
while [ $n -lt 3 ]; do
  n=$((n + 1))
  $WEX sh -c "printf '%s' '$COPY_TEXT' | wl-copy" >/dev/null 2>&1 || true
  sleep 2
done
sleep 2
# every ServerCutText the client saw should be exactly one payload long
lens=$(docker logs vncclient 2>&1 | sed -n 's/^recv ServerCutText: \([0-9]*\) bytes.*/\1/p' | sort -u | tr '\n' ' ')
check "distinct ServerCutText lengths over 3 copies" "$(echo $lens)" "$SHORT_LEN"

docker rm -f westonc vncclient >/dev/null 2>&1 || true
[ $fail -eq 0 ] && echo "PASS" || { echo "FAIL"; exit 1; }
