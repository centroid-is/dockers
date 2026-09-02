# Re-checking the patch yourself

`ra2_all.py` is a client port of noVNC's `core/ra2.js` — same RSA-PKCS1-v1.5
key exchange, same SHA-1/SHA-256 session keys, same AES-EAX framing with the
16-byte little-endian counter nonce. It takes the security type as an argument
so all four RSA-AES variants can be exercised against a running server, and it
asserts the session is encrypted for 5/129 and plaintext for 6/130.

```sh
docker build -t weston-ra2ne ..                 # from weston/
docker network create vnctest
docker run -d --name westonr --network vnctest --user root weston-ra2ne sh -c \
  "echo 'centroid:foo' | chpasswd && mkdir -p /tmp/xdg && chown centroid /tmp/xdg \
   && chmod 700 /tmp/xdg && exec su -s /bin/bash -c \
   'XDG_RUNTIME_DIR=/tmp/xdg exec weston -c /home/centroid/.config/weston.ini \
    -B vnc --width=1280 --height=800' centroid"
```

Weston 16 has no `fullscreen-shell.so`, so this runs the image's own
weston.ini with only the VNC backend selected: `/usr/local/bin/weston` finds
no connected DRM connector, drops the `mirror-of` line, and adds
`--disable-transport-layer-security` because weston.ini configures no
certificate.

```sh
docker run --rm --network vnctest -v "$PWD:/w" python:3.12-slim sh -c \
  "pip -q install pycryptodome && python /w/ra2_all.py westonr 5900 centroid foo 6"
```

Swap the trailing `6` for 5, 129 or 130. The server exits when the client
disconnects, so restart it between runs.
