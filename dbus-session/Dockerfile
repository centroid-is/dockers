FROM alpine:latest

RUN apk add --no-cache dbus tini

# Create a user to run the session D-Bus daemon
RUN adduser -D -u 1000 centroid

# Create the runtime directory for the session bus
RUN mkdir -p /run/user/1000 && chown centroid:centroid /run/user/1000

USER centroid

ENTRYPOINT ["/sbin/tini", "--"]
CMD ["dbus-daemon", "--session", "--nofork", "--nosyslog", "--address=unix:path=/run/user/1000/bus", "--print-address"]