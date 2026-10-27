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

# Per-station settings. One image serves every HMI, so they cannot be baked
# in; they are written here for the page to fetch, in the same shape as
# defaults.json.
#
# NOVNC_STATION_NAME: unset means the tab just reads "CentroidX".
#
# NOVNC_SHARED_CLIPBOARD: the operator's clipboard and the station's follow
# each other unless this is off. Off is forced on every browser, the way
# mandatory.json would; on leaves ?shared_clipboard=false in the URL free to
# switch it off for one browser. Anything unrecognised stops the container,
# since a typo that silently left the clipboard shared is the wrong failure.
: "${NOVNC_STATION_NAME:=}"
case "$(printf '%s' "${NOVNC_SHARED_CLIPBOARD:-on}" | tr '[:upper:]' '[:lower:]')" in
    on|true|yes|1)   SHARED_CLIPBOARD=true ;;
    off|false|no|0)  SHARED_CLIPBOARD=false ;;
    *)
        echo "novnc: NOVNC_SHARED_CLIPBOARD='${NOVNC_SHARED_CLIPBOARD}' is neither on nor off" >&2
        exit 1 ;;
esac
printf '{"station":"%s","shared_clipboard":%s}\n' \
    "$NOVNC_STATION_NAME" "$SHARED_CLIPBOARD" > /opt/novnc/branding.json
if [ -n "$NOVNC_STATION_NAME" ]; then
    echo "novnc: station name '$NOVNC_STATION_NAME'"
fi
if [ "$SHARED_CLIPBOARD" = false ]; then
    echo "novnc: shared clipboard off"
fi

exec websockify \
    --web /opt/novnc \
    --cert "$PEM" \
    "0.0.0.0:${NOVNC_PORT:-6080}" \
    "${NOVNC_TARGET:-weston:5900}"
