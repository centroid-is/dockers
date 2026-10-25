#!/bin/bash
# usage: run.sh IMAGE   (IMAGE built from ./Dockerfile; also needs an "rfbpy"
# image: python:3.12-slim with pycryptodome). Prints weston's exit status:
# 139 is the crash, "running" is the fix.
IMG=$1; N=vnc-repro; W=westonr
docker rm -f $W >/dev/null 2>&1; docker network rm $N >/dev/null 2>&1
docker network create $N >/dev/null
docker run -d --platform linux/amd64 --name $W --network $N --user root $IMG sh -c \
  "echo 'centroid:foo' | chpasswd && mkdir -p /tmp/xdg && chown centroid /tmp/xdg \
   && chmod 700 /tmp/xdg && cd /home/centroid && ulimit -c unlimited && exec su -s /bin/bash -c \
   'ulimit -c unlimited; XDG_RUNTIME_DIR=/tmp/xdg exec weston -c /home/centroid/.config/weston.ini \
    -B vnc --renderer=pixman' centroid" >/dev/null
for i in $(seq 60); do docker logs $W 2>&1 | grep -q "launching '/usr/libexec/weston-desktop-shell'" && break; sleep 1; done
sleep 4
docker exec -d -e XDG_RUNTIME_DIR=/tmp/xdg -e WAYLAND_DISPLAY=wayland-1 --user centroid $W sh -c 'cursor-share > /tmp/client.log 2>&1'
sleep 4
P="docker run --rm --network $N -v $PWD:/w rfbpy python -u /w/pointer.py $W 5900 centroid foo"
# A: enter the window, left half -> set_cursor(S) on A's pointer; stays 25 s
$P m100,100 s1 m120,110 s25 > a.log 2>&1 &
sleep 10
# B: connects while A is still on; enters -> set_cursor(S) on B's pointer
$P m200,200 s1 m210,210 s24 m500,200 s1 m510,210 s3 > b.log 2>&1 &
# A closes at ~t+26 (its seat is parked, pointer released -> sprite unmapped);
# B crosses to the right half at ~t+26..27 -> hotspot change on the same surface
wait
sleep 2
echo "== weston state: $(docker inspect -f '{{.State.Status}} exit={{.State.ExitCode}}' $W)"
echo "== a"; cat a.log; echo "== b"; cat b.log
echo "== client"; docker exec $W cat /tmp/client.log 2>/dev/null || docker cp $W:/tmp/client.log - 2>/dev/null | tar -xO
echo "== weston tail"; docker logs $W 2>&1 | grep -vE "^ +|could not load|none: No such" | tail -12
