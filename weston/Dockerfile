FROM debian:trixie-slim
ARG SEAT_GID=995

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       weston seatd openssl libgl1-mesa-dri libgbm1 \
       maliit-keyboard qtwayland5 \
    && rm -rf /var/lib/apt/lists/*

# Custom Maliit keyboard QML to make it 70% width and centered
COPY Keyboard.qml /usr/lib/x86_64-linux-gnu/maliit/keyboard2/qml/Keyboard.qml

# Custom Maliit device config to reduce keyboard height to 30%
COPY default.json /usr/share/maliit/keyboard2/devices/default.json

# Create default user 'centroid'
RUN useradd -m -s /bin/bash centroid \
    && groupadd -g ${SEAT_GID} seat \
    && usermod -aG seat,video,tty centroid

# 3. Copy in your weston.ini
#    ensure config dir exists
RUN mkdir -p /home/centroid/.config
COPY weston.ini /home/centroid/.config/weston.ini

# 4. Copy Maliit Solarized themes
RUN mkdir -p /home/centroid/.local/share/maliit/keyboard2/themes/solarized-dark
RUN mkdir -p /home/centroid/.local/share/maliit/keyboard2/themes/solarized-light
COPY maliit-theme-dark/main.ini /home/centroid/.local/share/maliit/keyboard2/themes/solarized-dark/main.ini
COPY maliit-theme-dark/extended-keys.ini /home/centroid/.local/share/maliit/keyboard2/themes/solarized-dark/extended-keys.ini
COPY maliit-theme-light/main.ini /home/centroid/.local/share/maliit/keyboard2/themes/solarized-light/main.ini
COPY maliit-theme-light/extended-keys.ini /home/centroid/.local/share/maliit/keyboard2/themes/solarized-light/extended-keys.ini

# 5. Set default theme to solarized-dark using dconf
RUN mkdir -p /home/centroid/.config/dconf \
    && echo "[org/maliit/keyboard/maliit]" > /home/centroid/.config/dconf/user.txt \
    && echo "theme='solarized-dark'" >> /home/centroid/.config/dconf/user.txt

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