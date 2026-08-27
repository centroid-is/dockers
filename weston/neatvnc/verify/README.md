# Re-checking the patch yourself

`ra2_all.py` is a client port of noVNC's `core/ra2.js` — same RSA-PKCS1-v1.5
key exchange, same SHA-1/SHA-256 session keys, same AES-EAX framing with the
16-byte little-endian counter nonce. It takes the security type as an argument
so all four RSA-AES variants can be exercised against a running server, and it
asserts the session is encrypted for 5/129 and plaintext for 6/130.

```sh
docker build -t weston-ra2ne ..                 # from weston/
docker network create vnctest
docker run -d --name westonr --network vnctest \
  -e XDG_RUNTIME_DIR=/tmp/xdg --user root weston-ra2ne sh -c \
  "echo 'centroid:foo' | chpasswd && mkdir -p /tmp/xdg && chown centroid /tmp/xdg \
   && chmod 700 /tmp/xdg && exec su -s /bin/bash -c \
   'XDG_RUNTIME_DIR=/tmp/xdg exec weston --backend=vnc-backend.so \
    --disable-transport-layer-security --shell=fullscreen-shell.so --no-config \
    --width=1280 --height=800' centroid"

docker run --rm --network vnctest -v "$PWD:/w" python:3.12-slim sh -c \
  "pip -q install pycryptodome && python /w/ra2_all.py westonr 5900 centroid foo 6"
```

Swap the trailing `6` for 5, 129 or 130. Note the server quits when a client
disconnects under `--no-config`, so restart it between runs.
