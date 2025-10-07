FROM debian:trixie-slim

RUN apt-get update && apt-get install -y --no-install-recommends gnome-keyring tini && rm -rf /var/lib/apt/lists/*

# Create a user to run the gnome-keyring daemon
RUN useradd -m -u 1000 centroid

USER centroid

ENTRYPOINT ["/usr/bin/tini", "--"]

CMD ["/bin/sh", "-c", "gnome-keyring-daemon --unlock < /run/secrets/keyring_password; exec gnome-keyring-daemon --start --components=secrets --foreground"]