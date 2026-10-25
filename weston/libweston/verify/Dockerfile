# BASE: the image under test, e.g. ghcr.io/centroid-is/weston:latest or a local build
ARG BASE=ghcr.io/centroid-is/weston:latest
FROM ${BASE}
USER root
RUN apt-get update && apt-get install -y --no-install-recommends gcc libc6-dev libwayland-dev wayland-protocols pkg-config && rm -rf /var/lib/apt/lists/*
COPY cursor-share.c /src/
RUN set -eux; cd /src; \
    xdg="$(pkg-config --variable=pkgdatadir wayland-protocols)/stable/xdg-shell/xdg-shell.xml"; \
    wayland-scanner client-header "$xdg" xdg-shell-client-protocol.h; \
    wayland-scanner private-code "$xdg" xdg-shell-protocol.c; \
    gcc -O1 -Wall -o /usr/local/bin/cursor-share cursor-share.c xdg-shell-protocol.c -I. $(pkg-config --cflags --libs wayland-client)
