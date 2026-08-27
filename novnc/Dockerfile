# noVNC + websockify, serving wss:// in front of weston's screen-share VNC.
#
# Three files are layered over the upstream checkout:
#   defaults.json  - scaling, autoconnect, reconnect
#   index.html     - upstream has none, so "/" would be a directory listing
#   vnc.html       - overrides UI methods to remember credentials in
#                    localStorage and pin the server's RSA key (TOFU).
#                    app/ and core/ stay stock, so upgrades drop straight in.
FROM python:3.12-slim

ARG NOVNC_REF=v1.7.0

RUN apt-get update \
    && apt-get install -y --no-install-recommends git ca-certificates openssl \
    && rm -rf /var/lib/apt/lists/* \
    && pip install --no-cache-dir websockify \
    && git clone --depth 1 --branch ${NOVNC_REF} \
       https://github.com/novnc/noVNC /opt/novnc \
    && rm -rf /opt/novnc/.git

COPY defaults.json index.html vnc.html /opt/novnc/
COPY entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh

EXPOSE 6080
VOLUME /certs

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
