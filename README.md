# dbus session
Running

```bash
docker run --rm -it -v ./tmp-session:/tmp ghcr.io/centroid-is/dbus-session:latest
```

# gnome keyring
Generate keyring password 
```bash
openssl rand -base64 32 > keyring_password
```

Running

```bash
docker run --rm -it -v ./tmp-session:/run/dbus -v ./pass:/run/secrets -e DBUS_SESSION_BUS_ADDRESS="unix:path=/run/dbus/dbus-session" ghcr.io/centroid-is/gnome-keyring:latest
```