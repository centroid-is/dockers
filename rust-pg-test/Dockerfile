# Build and test image for Rust projects whose tests run against PostgreSQL 17.
#
# What a CI job would otherwise fetch on every run is already here:
#   - the stable Rust toolchain with a C toolchain, OpenSSL headers and pkg-config
#     (rust:1-bookworm is built on buildpack-deps)
#   - cargo-nextest
#   - the PostgreSQL 17 client tools (pg_dump, pg_restore, psql, dropdb): Debian
#     bookworm ships 15, and dump/restore tests need the server's major version
#
# No project code is in the image. A job runs in it and brings its own checkout;
# the PostgreSQL server is a separate container (postgres:17).
FROM rust:1-bookworm

RUN apt-get update \
 && apt-get install -y --no-install-recommends postgresql-common ca-certificates \
 && /usr/share/postgresql-common/pgdg/apt.postgresql.org.sh -y \
 && apt-get install -y --no-install-recommends postgresql-client-17 \
 && rm -rf /var/lib/apt/lists/*

# linux/amd64 only: the nextest archive below is the x86_64 build.
RUN curl -LsSf https://get.nexte.st/latest/linux | tar zxf - -C /usr/local/cargo/bin

# The build fails here, not in a job, if a tool is missing.
RUN rustc --version && cargo --version && cargo nextest --version \
 && pg_dump --version && pkg-config --exists openssl

LABEL org.opencontainers.image.source="https://github.com/centroid-is/dockers" \
      org.opencontainers.image.description="Rust stable, cargo-nextest and PostgreSQL 17 client tools for CI"
