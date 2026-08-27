# =============================================================================
# Stage 1: build the CentroidX on-screen keyboard
#
# keyboard/keyboard.c is a fork of weston's own clients/keyboard.c, so it is
# built inside the matching weston source tree: check out the same version the
# runtime image installs, drop our file over the stock one, and build just the
# weston-keyboard target.
# =============================================================================
FROM debian:trixie-slim AS keyboard-builder

ARG WESTON_VERSION=14.0.2

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

COPY keyboard/keyboard.c /src/weston/clients/keyboard.c

# Only the client is wanted; every backend is disabled so the build stays
# small and does not need GPU/seat libraries it will never use.
RUN cd /src/weston \
    && meson setup build \
       -Dbackend-default=headless \
       -Dbackend-drm=false -Dbackend-rdp=false -Dbackend-vnc=false \
       -Dbackend-pipewire=false -Dbackend-x11=false \
       -Dxwayland=false -Dremoting=false -Dpipewire=false \
       -Dtests=false -Ddoc=false \
    && ninja -C build clients/weston-keyboard

# =============================================================================
# Stage 2: build neatvnc with the RA2ne security types
#
# Debian's neatvnc offers a browser client no security type it can use, so the
# only overlap with noVNC is Apple DH — unauthenticated Diffie-Hellman. The
# patch adds RA2ne (types 6 and 130), which hands the client the server's RSA
# key to verify instead. See neatvnc/README.md.
# =============================================================================
FROM debian:trixie-slim AS neatvnc-builder

ARG NEATVNC_VERSION=v0.9.1

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
FROM debian:trixie-slim
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
COPY --from=neatvnc-builder /src/neatvnc/build/libneatvnc.so.0 /tmp/libneatvnc.so.0
RUN set -eux; \
    target=$(ls /usr/lib/*/libneatvnc.so.0); \
    test -f "$target"; \
    cp /tmp/libneatvnc.so.0 "$target"; \
    rm /tmp/libneatvnc.so.0; \
    ldconfig

# 3. Copy in your weston.ini
#    ensure config dir exists
RUN mkdir -p /home/centroid/.config
COPY weston.ini /home/centroid/.config/weston.ini

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
