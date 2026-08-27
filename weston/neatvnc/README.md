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

## Rebuilding

The Dockerfile builds neatvnc from the v0.9.1 tag with the patch applied and
installs the result over Debian's `libneatvnc.so.0`. Weston is not rebuilt —
the patch adds no symbols and changes no struct that crosses the ABI, so
Debian's weston binary links against it unchanged.
