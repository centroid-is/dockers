FROM debian:bookworm-slim

RUN apt-get update \
    && apt-get install -y --no-install-recommends weston openssl \
    && rm -rf /var/lib/apt/lists/*

# Enable testing repo, update weston from testing, then disable testing repo
RUN echo "deb http://deb.debian.org/debian testing main" \
    > /etc/apt/sources.list.d/testing.list \
    && apt-get update \
    && apt-get install -y --no-install-recommends -t testing weston \
    && rm /etc/apt/sources.list.d/testing.list \
    && apt-get update \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

# Create default user 'centroid'
RUN useradd -m -s /bin/bash centroid

# 3. Copy in your weston.ini
#    ensure config dir exists and is owned by centroid
RUN mkdir -p /home/centroid/.config \
    && chown centroid:centroid /home/centroid/.config
COPY weston.ini /home/centroid/.config/weston.ini
RUN chown centroid:centroid /home/centroid/.config/weston.ini

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

# Switch to centroid and set working dir
USER centroid
WORKDIR /home/centroid

# Default command
CMD ["weston"]