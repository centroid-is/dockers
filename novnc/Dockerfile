# noVNC + websockify, serving wss:// in front of weston's screen-share VNC.
#
# Three files are layered over the upstream checkout:
#   defaults.json  - scaling, autoconnect, reconnect
#   index.html     - upstream has none, so "/" would be a directory listing
#   vnc.html       - overrides UI methods to remember credentials in
#                    localStorage and pin the server's RSA key (TOFU).
#
# app/ and core/ are stock apart from patches/, which carries the shared
# clipboard until a noVNC release has it. See patches/README.md before
# moving NOVNC_REF.
FROM python:3.12-slim

ARG NOVNC_REF=v1.7.0

RUN apt-get update \
    && apt-get install -y --no-install-recommends git ca-certificates openssl \
    && rm -rf /var/lib/apt/lists/* \
    && pip install --no-cache-dir websockify \
    && git clone --depth 1 --branch ${NOVNC_REF} \
       https://github.com/novnc/noVNC /opt/novnc \
    && rm -rf /opt/novnc/.git

# Applied in order, and the build fails if one does not apply: a patch that
# half-applies to a newer noVNC is worse than no image.
COPY patches/ /tmp/novnc-patches/
RUN cd /opt/novnc \
    && for p in /tmp/novnc-patches/*.patch; do \
         echo "novnc: applying $(basename "$p")"; \
         git apply "$p" || exit 1; \
       done \
    && rm -rf /tmp/novnc-patches

COPY defaults.json mandatory.json index.html /opt/novnc/

# vnc.html is patched at build time from the pinned checkout, never shipped
# as a hand-edited copy — see patch-vnc-html.py for why.
COPY centroid-mark.svg /opt/novnc/app/images/icons/centroid-mark.svg
COPY overrides.js /tmp/overrides.js
COPY patch-vnc-html.py /tmp/patch-vnc-html.py
RUN python3 /tmp/patch-vnc-html.py && rm /tmp/patch-vnc-html.py /tmp/overrides.js
COPY redirect.py /usr/local/bin/redirect.py
COPY entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh

EXPOSE 6080
VOLUME /certs

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
