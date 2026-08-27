# Patched neatvnc

Debian ships neatvnc 0.9.1, which offers a browser client no security type it
can use. noVNC implements RA2ne (RFB security type 6) and nothing else from the
RSA-AES family; stock neatvnc offers only the encrypting variants (5 and 129)
plus Apple DH (30). The overlap is Apple DH — unauthenticated Diffie-Hellman,
where the client cannot verify the server at all.

`0001-Add-the-RA2ne-security-types-6-and-130.patch` adds types 6 and 130 so a
browser gets the server's RSA key to verify instead. It applies to the v0.9.1
tag and is +62/-2 across 8 files, nearly all of it reusing neatvnc's existing
RSA-AES handshake.

The types only appear when weston is run **without** `--vnc-tls-cert`, because
they sit behind the same `NVNC_AUTH_REQUIRE_ENCRYPTION` guard as Apple DH. With
TLS configured the offered list is byte-for-byte what stock neatvnc offers.

## What this does and does not buy

RA2ne gives the client the server's RSA public key to verify, where Apple DH
gives it nothing to verify at all. That is the whole point of the change.

It is **not** SSH-style trust-on-first-use yet, because the key is not stable:

```
run 1:                      89-4d-a6-13-04-f1-99-30
run 1 again (same process): 89-4d-a6-13-04-f1-99-30
run 2 (after restart):      c2-2c-da-81-c6-ab-b4-91
```

neatvnc generates the RSA keypair lazily in memory on the first RA2ne
connection (`src/auth/rsa-aes.c`) and only persists it if the application calls
`nvnc_set_rsa_creds()`. **Weston never does** — not in 14.0.2, and not in
`main` (16.x), whose `vnc.c` does not contain the string "rsa" at all. So the
fingerprint changes on every weston restart, and "verify the fingerprint"
degrades to "click approve".

Fixing that is a *weston* patch, not a neatvnc one: the library has exposed
`nvnc_set_rsa_creds(struct nvnc*, const char* private_key_path)` since before
0.9.1, and weston simply needs an option — `--vnc-rsa-key=FILE` — to call it.
That is the natural follow-up to this PR, and it is what turns the fingerprint
prompt into something worth reading.

## Browser clients need a secure context

noVNC's RA2ne path calls `window.crypto.subtle`, which browsers only expose
over HTTPS or on localhost. Served over plain `http://`, the client dies with

```
TypeError: Cannot read properties of undefined (reading 'digest')
    at RFB.serverVerify (app/ui.js)
    at RSAAESAuthenticationState.negotiateRA2neAuthAsync (core/ra2.js)
```

So the websockify front end **must** serve `wss://`. This is a useful property
rather than a nuisance: the insecure deployment does not silently degrade, it
refuses to run.

## Not upstreamed

Deliberately. Review it properly before it goes anywhere near any1/neatvnc.

## Verified

Against this image, with `--disable-transport-layer-security`:

| check | result |
|---|---|
| types offered | `129, 5, 130, 6, 30` (was `129, 5, 30`) |
| noVNC's RA2ne algorithm, ported from `core/ra2.js` | handshake + plaintext session + framebuffer |
| ...same, through a websockify/noVNC container | identical |
| RA2 (5), RA2_256 (129) | still encrypt the session end to end |
| RA2ne_256 (130) | handshake + plaintext session |
| Apple DH (30) | unchanged |
| TigerVNC 1.15 | names 6 and 130 correctly, still picks RA2_256 (129) |
| with `--vnc-tls-cert` | offers `19, 129, 5`; VeNCrypt X509Plain unchanged |
| neatvnc's own `meson test` | 2/2 pass |

## Verified on hardware

Built and run on the `housecontrol-hmi` rig (10.50.10.11, x86_64 Debian
trixie), against the real DRM + `screen-share` configuration rather than a
synthetic one:

- the screen-share VNC offered `129, 5, 130, 6, 30`
- all four RSA-AES variants completed, encrypted for 5/129 and plaintext for
  6/130
- Apple DH unchanged; TigerVNC still chose RA2_256
- with `--vnc-tls-cert`, still `19, 129, 5` + X509Plain
- a real browser connected over `wss://` through noVNC and drove the live HMI

## Rebuilding

The Dockerfile builds neatvnc from the v0.9.1 tag with the patch applied and
installs the result over Debian's `libneatvnc.so.0`. Weston is not rebuilt —
the patch adds no symbols and changes no struct that crosses the ABI, so
Debian's weston binary links against it unchanged.
