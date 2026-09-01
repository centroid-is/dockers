# =============================================================================
# Stage 1: the weston source tree both weston stages build out of
#
# Two artifacts are built from weston's own sources — the forked on-screen
# keyboard and the patched VNC backend — so the checkout and the build
# dependencies are shared rather than installed twice.
# =============================================================================
FROM debian:forky-slim AS weston-src

ARG WESTON_VERSION=16.0.0

# build-dep on the distro's own weston package pulls exactly the set this
# source tree expects — hand-listing the -dev packages drifts with every
# weston release.
RUN sed -i 's/^Types: deb$/Types: deb deb-src/' /etc/apt/sources.list.d/debian.sources \
    && apt-get update \
    && apt-get build-dep -y weston \
    && apt-get install -y --no-install-recommends ca-certificates git \
    && rm -rf /var/lib/apt/lists/*

RUN git clone --depth 1 --branch ${WESTON_VERSION} \
    https://gitlab.freedesktop.org/wayland/weston.git /src/weston


# =============================================================================
# Stage 1a: build the CentroidX on-screen keyboard
#
# keyboard/keyboard.c is a fork of weston's own clients/keyboard.c, so it is
# built inside the matching weston source tree: drop our file over the stock
# one and build just the weston-keyboard target.
# =============================================================================
FROM weston-src AS keyboard-builder

COPY keyboard/keyboard.c /src/weston/clients/keyboard.c

# Only the client is wanted; every backend is disabled so the build stays
# small and does not need GPU/seat libraries it will never use.
RUN cd /src/weston \
    && meson setup build \
       -Dbackend-default=headless \
       -Dbackend-drm=false -Dbackend-rdp=false -Dbackend-vnc=false \
       -Dbackend-pipewire=false -Dbackend-x11=false \
       -Dxwayland=false -Ddeprecated-remoting=false -Ddeprecated-pipewire=false \
       -Dtests=false -Ddoc=false \
    && ninja -C build clients/weston-keyboard


# =============================================================================
# Stage 1b: build the VNC backend with output capture serviced
#
# Stock weston never services a screen-capture task filed against the VNC
# output, and aborts the whole compositor on the assert that catches it. The
# patch is two guards in vnc_output_repaint(), both of them lifted from what
# backend-drm and backend-pipewire already do; see vnc-backend/README.md.
#
# Only vnc-backend.so is taken, over Debian's copy of the same module, the way
# the patched libneatvnc is. It links against libweston's *private* ABI, so it
# has to be built from the same sources the runtime image installs: same
# 16.0.0 tag, same debian:forky-slim, and the meson options Debian itself
# configures with (debian/rules) so config.h comes out identical.
# =============================================================================
FROM weston-src AS vnc-backend-builder

COPY vnc-backend/*.patch /patches/

RUN cd /src/weston \
    && git apply --check /patches/*.patch \
    && git apply /patches/*.patch \
    && meson setup build \
       -Dbackend-rdp=true -Dbackend-vnc=true -Drenderer-vulkan=true \
       -Dsystemd=true -Ddoc=false \
    && ninja -C build libweston/backend-vnc/vnc-backend.so

# =============================================================================
# Stage 2: build neatvnc with the RA2ne security types
#
# Debian's neatvnc offers a browser client no security type it can use, so the
# only overlap with noVNC is Apple DH — unauthenticated Diffie-Hellman. The
# patch adds RA2ne (types 6 and 130), which hands the client the server's RSA
# key to verify instead. Still absent from upstream 1.0.1, so the stage stays.
# See neatvnc/README.md.
# =============================================================================
FROM debian:forky-slim AS neatvnc-builder

ARG NEATVNC_VERSION=v1.0.1

# As with the keyboard stage, build-dep on the distro package rather than
# hand-listing -dev packages that drift between releases.
RUN sed -i 's/^Types: deb$/Types: deb deb-src/' /etc/apt/sources.list.d/debian.sources \
    && apt-get update \
    && apt-get build-dep -y neatvnc \
    && apt-get install -y --no-install-recommends ca-certificates git \
    && rm -rf /var/lib/apt/lists/*

RUN git clone --depth 1 --branch ${NEATVNC_VERSION} \
    https://github.com/any1/neatvnc.git /src/neatvnc

COPY neatvnc/*.patch /patches/

RUN cd /src/neatvnc \
    && git apply --check /patches/*.patch \
    && git apply /patches/*.patch \
    && meson setup build --buildtype=release -Dtls=enabled -Dnettle=enabled \
    && ninja -C build


# =============================================================================
# Stage 3: runtime
# =============================================================================
FROM debian:forky-slim
ARG SEAT_GID=995

RUN apt-get update \
    && apt-get install -y --no-install-recommends weston seatd openssl libgl1-mesa-dri libgbm1 fonts-roboto \
    && rm -rf /var/lib/apt/lists/*


# Create default user 'centroid'
RUN useradd -m -s /bin/bash centroid \
    && groupadd -g ${SEAT_GID} seat \
    && usermod -aG seat,video,tty centroid

# The restyled on-screen keyboard, referenced by weston.ini's [input-method].
COPY --from=keyboard-builder /src/weston/build/clients/weston-keyboard \
     /usr/local/bin/centroidx-keyboard
RUN chmod 755 /usr/local/bin/centroidx-keyboard

# The patched libneatvnc, over Debian's copy. Weston itself is not rebuilt:
# the patch adds no symbols and changes nothing that crosses the ABI.
COPY --from=neatvnc-builder /src/neatvnc/build/libneatvnc.so.1 /tmp/libneatvnc.so.1
RUN set -eux; \
    target=$(ls /usr/lib/*/libneatvnc.so.1); \
    test -f "$target"; \
    cp /tmp/libneatvnc.so.1 "$target"; \
    rm /tmp/libneatvnc.so.1; \
    ldconfig

# The patched vnc-backend.so, over Debian's copy of the same module. Without
# it, any weston-screenshooter run aborts the compositor: it captures every
# output, and nothing on the VNC output ever services the task.
COPY --from=vnc-backend-builder \
     /src/weston/build/libweston/backend-vnc/vnc-backend.so /tmp/vnc-backend.so
RUN set -eux; \
    target=$(ls /usr/lib/*/libweston-16/vnc-backend.so); \
    test -f "$target"; \
    cp /tmp/vnc-backend.so "$target"; \
    rm /tmp/vnc-backend.so

# 3. Copy in your weston.ini
#    ensure config dir exists
RUN mkdir -p /home/centroid/.config
COPY weston.ini /home/centroid/.config/weston.ini

# weston.ini's mirror-of key needs the station's own DRM connector name, so it
# ships as a placeholder and is resolved at start-up. The resolver is installed
# as `weston`, ahead of /usr/bin/weston on PATH, so the `exec weston -c ...`
# entrypoint every station already has in its docker-compose.yml keeps working
# unchanged.
COPY weston-wrapper.sh /usr/local/bin/weston
RUN chmod 755 /usr/local/bin/weston

# Generate self-signed certs for VNC
RUN mkdir -p /home/centroid/vnc/certs \
    && openssl genrsa -out /home/centroid/vnc/certs/tls.key 2048 \
    && openssl req -new \
    -key /home/centroid/vnc/certs/tls.key \
    -out /home/centroid/vnc/certs/tls.csr \
    -subj "/C=IS/ST=Höfuðborgar Svæðið/L=Reykjavik/O=Centroid" \
    && openssl x509 -req \
    -days 365000 \
    -signkey /home/centroid/vnc/certs/tls.key \
    -in /home/centroid/vnc/certs/tls.csr \
    -out /home/centroid/vnc/certs/tls.crt \
    && chown -R centroid:centroid /home/centroid/vnc \
    && apt-get purge -y --auto-remove openssl

RUN chown -R centroid:centroid /home/centroid/

# Switch to centroid and set working dir
USER centroid
WORKDIR /home/centroid

EXPOSE 5900

# Default command
CMD ["weston", "-c", "/home/centroid/.config/weston.ini"]
