#!/bin/sh
# Generate a per-station self-signed cert on first start. Baking one into the
# image would give every station the same key, which is worse than useless.
set -eu

CERT_DIR="${NOVNC_CERT_DIR:-/certs}"
PEM="$CERT_DIR/novnc.pem"

if [ ! -f "$PEM" ]; then
    echo "novnc: no certificate at $PEM, generating a self-signed one"
    mkdir -p "$CERT_DIR"
    CN="${NOVNC_CERT_CN:-$(hostname)}"
    openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
        -subj "/C=IS/O=Centroid/CN=${CN}" \
        -addext "subjectAltName=DNS:${CN},DNS:localhost,IP:127.0.0.1" \
        -keyout "$CERT_DIR/novnc.key" -out "$CERT_DIR/novnc.crt" 2>/dev/null
    cat "$CERT_DIR/novnc.crt" "$CERT_DIR/novnc.key" > "$PEM"
    chmod 600 "$CERT_DIR/novnc.key" "$PEM"
fi

exec websockify \
    --web /opt/novnc \
    --cert "$PEM" \
    "0.0.0.0:${NOVNC_PORT:-6080}" \
    "${NOVNC_TARGET:-weston:5900}"
