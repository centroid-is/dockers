# dbus session
Running

```bash
docker run --rm -it -v ./tmp-session:/run/user/1000 ghcr.io/centroid-is/dbus-session:latest
```

# gnome keyring
Generate keyring password 
```bash
openssl rand -base64 32 > keyring_password
```

Running

```bash
docker run --rm -it -v ./tmp-session:/run/user/1000 -v ./pass:/run/secrets -e DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/1000/bus" ghcr.io/centroid-is/gnome-keyring:latest
```

```bash
ls -alh /home/centroid/.local/share/keyrings/
total 16K
drwx------ 2 centroid centroid 4.0K Oct  7 13:24 .
drwx------ 3 centroid centroid 4.0K Oct  7 13:24 ..
-rw------- 1 centroid centroid  105 Oct  7 13:24 login.keyring
-rw------- 1 centroid centroid  207 Oct  7 13:24 user.keystore
```