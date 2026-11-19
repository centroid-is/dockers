# dockers

Container images for CentroidX stations.

## weston: keyboard layouts

Every keyboard on a station types in the same set of layouts: a keyboard
plugged into the panel, each VNC client (noVNC in a browser included) and the
on-screen keyboard. Three are supported: Icelandic (`is`), English (`en`, xkb
`us`) and Polish (`pl`).

The weston container reads three environment variables at start-up:

| Variable | Default | Meaning |
|---|---|---|
| `KEYBOARD_LAYOUTS` | `is,en,pl` | Comma list of the layouts offered. `us` is accepted for `en`; unknown names are logged and skipped. |
| `KEYBOARD_DEFAULT` | *(empty)* | The layout every keyboard starts in. Empty means: the host's locale, else the host's keyboard layout, else the first of `KEYBOARD_LAYOUTS`. A default that is not in the list is added to it. |
| `KEYBOARD_OPTIONS` | `grp:alt_shift_toggle` | xkb options. The default makes **Alt+Shift** cycle through the layouts. Set `grp:ctrl_shift_toggle` for Ctrl+Shift, or an empty value for no shortcut. |

The host is consulted through its `/etc/default`, mounted read-only:
`LC_ALL` or `LANG` from `/etc/default/locale` (`is_IS.UTF-8` gives `is`,
`pl_PL.UTF-8` gives `pl`, any `en_*` gives `en`), then `XKBLAYOUT` from
`/etc/default/keyboard`. A host value only counts if it names an enabled
layout. Mount the directory rather than the two files: Docker creates a
missing bind source as a directory, and one standing where the host's
`/etc/default/locale` should be breaks the host's own logins.

A station whose operating system was installed in English but whose operators
type another language has to say so with `KEYBOARD_DEFAULT`; the host locale
alone would make it start in English.

```yaml
services:
  weston:
    image: ghcr.io/centroid-is/weston:latest
    volumes:
      - /etc/default:/run/host/etc/default:ro
    environment:
      KEYBOARD_LAYOUTS: ${KEYBOARD_LAYOUTS:-is,en,pl}
      KEYBOARD_DEFAULT: ${KEYBOARD_DEFAULT:-}
```

The container log states what was chosen and why, e.g.

```
centroidx-weston: keyboard layouts pl,is,en (default pl from host locale pl_PL.UTF-8)
```

Each keyboard switches on its own: Alt+Shift on one VNC client does not move
the panel's keyboard or another client. Every new VNC connection starts in the
default layout.

To see what a station would resolve without starting anything:

```bash
docker run --rm -e WESTON_WRAPPER_DRY_RUN=1 \
  -e KEYBOARD_LAYOUTS=is,en,pl \
  -v /etc/default:/run/host/etc/default:ro \
  ghcr.io/centroid-is/weston:latest weston -c /home/centroid/.config/weston.ini
```

`sh weston/test/wrapper-test.sh` runs the resolution tests; CI runs them before
building the image.

## rust-pg-test: a CI image for Rust with PostgreSQL

`ghcr.io/centroid-is/rust-pg-test` holds what a Rust test job would otherwise
download on every run: the stable toolchain (with a C toolchain, OpenSSL headers
and pkg-config), `cargo-nextest` and the PostgreSQL 17 client tools (`pg_dump`,
`pg_restore`, `psql`). It holds no project code; the PostgreSQL server is a
separate `postgres:17` container.

```yaml
jobs:
  test:
    runs-on: self-hosted
    container:
      image: ghcr.io/centroid-is/rust-pg-test@sha256:<digest>
    services:
      postgres:
        image: postgres:17
        env:
          POSTGRES_PASSWORD: test
    steps:
      - uses: actions/checkout@v4
      - run: cargo nextest run --locked
```

Pin the image by digest (the build prints it) so a new stable Rust arrives when
the job's file says so, not in the middle of a release. Run the workflow by hand
to rebuild on a newer toolchain.
