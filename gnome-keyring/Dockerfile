FROM debian:trixie-slim

RUN apt-get update && apt-get install -y --no-install-recommends gnome-keyring tini && rm -rf /var/lib/apt/lists/*

ENTRYPOINT ["/usr/bin/tini", "--"]

CMD ["/bin/sh", "-c", "gnome-keyring-daemon --unlock < /run/secrets/keyring_password; exec gnome-keyring-daemon --start --components=secrets --foreground"]