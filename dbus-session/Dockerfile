FROM alpine:latest

RUN apk add --no-cache dbus tini

ENTRYPOINT ["/sbin/tini", "--"]
CMD ["dbus-daemon", "--session", "--nofork", "--nosyslog", "--address=unix:path=/tmp/dbus-session", "--print-address"]