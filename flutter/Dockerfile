FROM debian:bookworm-slim

# Install only the required libraries
RUN apt-get update && apt-get install -y --no-install-recommends \
    libstdc++6 \
    libgcc-s1 \
    libc6 \
    libfontconfig1 \
    libpthread-stubs0-dev \
    libxkbcommon0 \
    libwayland-client0 \
    libwayland-cursor0 \
    libwayland-egl1 \
    libegl1 \
    libfreetype6 \
    libexpat1 \
    libffi8 \
    libgl1 \
    zlib1g \
    libpng16-16 \
    libglib2.0-0 \ 
    libsecret-1-0 \
    sqlite3 \
    && rm -rf /var/lib/apt/lists/*

# Hacks for amplify secure storage
RUN ln -s /usr/lib/x86_64-linux-gnu/libgio-2.0.so.0 /usr/lib/x86_64-linux-gnu/libgio-2.0.so
RUN ln -s /usr/lib/x86_64-linux-gnu/libglib-2.0.so.0 /usr/lib/x86_64-linux-gnu/libglib-2.0.so
RUN ln -s /usr/lib/x86_64-linux-gnu/libsecret-1.so.0 /usr/lib/x86_64-linux-gnu/libsecret-1.so

RUN useradd -m centroid

# Copy in your Flutter libraries and executable
# These will be supplied at build time
ARG APP_DIR=/flutter-app
WORKDIR $APP_DIR

# You can use build args to specify the location of your app and libs
# Example: docker build --build-arg APP_BIN=flutter_elinux_wayland --build-arg FLUTTER_ENGINE=./bundle ...
ARG APP_BIN
ENV BINARY=${APP_BIN}
ARG APP_BUNDLE
ENV BUNDLE=${APP_BUNDLE}

COPY ${APP_BUNDLE} $APP_DIR/

RUN chown -R centroid:centroid $APP_DIR \
    && chmod +x $APP_DIR/${BINARY}

USER centroid
CMD ["/bin/sh", "-c", "./${BINARY} --bundle=. --fullscreen --onscreen-keyboard"]
