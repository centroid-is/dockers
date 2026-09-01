# Re-checking the clipboard patch yourself

Two RFB clients, both starting from the same RA2ne handshake as
`../../neatvnc/verify/ra2_all.py`. `clipboard.py` speaks the legacy
`ClientCutText`/`ServerCutText` pair; `clipboard_ext.py` negotiates the
extended clipboard pseudo-encoding, which is the path noVNC actually takes.
Each sends one clipboard update and then prints every one the server pushes
back, until its deadline expires.

Build the image and put a compositor on a network:

```sh
docker build -t weston-clip ..                  # from weston/
docker network create vnctest
docker run -d --name westonc --network vnctest --user root weston-clip sh -c \
  "echo 'centroid:foo' | chpasswd && mkdir -p /tmp/xdg && chown centroid /tmp/xdg \
   && chmod 700 /tmp/xdg && exec su -s /bin/bash -c \
   'XDG_RUNTIME_DIR=/tmp/xdg exec weston -c /home/centroid/.config/weston.ini \
    -B headless,vnc --fake-seat --width=1280 --height=800' centroid"
```

`-B headless,vnc --fake-seat` is the point of the exercise, not a shortcut.
The headless backend brings up a seat named `default` at start-up and the VNC
backend adds a `VNC Client` seat only when a peer connects — the same split a
station has between its DRM seat and the remote one, and the reason the patch
publishes on every seat rather than on the peer's.

The Wayland side needs `wl-clipboard`, which the runtime image does not ship:

```sh
docker build -t weston-clip-test - <<'EOF'
FROM weston-clip
USER root
RUN apt-get update && apt-get install -y --no-install-recommends wl-clipboard \
    && rm -rf /var/lib/apt/lists/*
USER centroid
EOF
```

Run a client, holding the session open for 40s. The last argument is the text
to send, as hex; `@N` in `clipboard.py` means N bytes of filler instead, for
sizes past a pipe buffer.

```sh
docker run -d --name vncclient --network vnctest -v "$PWD:/w" python:3.12-slim \
  sh -c "pip -q install pycryptodome \
         && python -u /w/clipboard.py westonc 5900 centroid foo 40 68656c6c6f"
```

Then, while it is still connected:

```sh
# what the client sent should be the selection, on the *other* seat
docker exec -e XDG_RUNTIME_DIR=/tmp/xdg -e WAYLAND_DISPLAY=wayland-1 \
  --user centroid westonc wl-paste -n

# and what a Wayland client copies should reach the VNC client
docker exec -e XDG_RUNTIME_DIR=/tmp/xdg -e WAYLAND_DISPLAY=wayland-1 \
  --user centroid westonc sh -c "printf hello-back | wl-copy"

docker logs vncclient
```

Add `WAYLAND_DEBUG=1` to the `wl-paste` invocation to see which `wl_seat` it
binds its data device to — it should be `"default"`, the seat that existed
before any VNC client connected.

`clipboard_ext.py` takes the same arguments and additionally prints the
server's advertised capabilities and, for each Provide message, the declared
length against the number of bytes actually inflated.

## `retained.sh`

`retained.sh [image]` drives both of the above and asserts byte **counts**,
which is the part that matters: a selection served truncated or doubled reads
correctly at a glance and is only caught by counting. It covers the selection
this backend keeps serving after the client that published it has gone (short
and 4 MiB), a requester killed mid-transfer, and three copies in a row on the
client-facing side. It brings its own `westonc`/`vncclient` containers up and
down, so it wants the `vnctest` network and the `wl-clipboard` image above,
and nothing else running under those names.

```sh
sh retained.sh                 # against weston-clip-test
sh retained.sh some-other-image
```
