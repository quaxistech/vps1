# Deployment

AltTech runs natively under systemd on the same host as altspotter. PostgreSQL
and NATS listen on loopback only, so a consumer — and AltTech is one — must be
on that host or reach it through an SSH tunnel.

## Where the build happens

**On a build host, never on the target.** The target runs a live market-data
collector on modest hardware; it should not spend CPU and RAM compiling, should
not carry a build toolchain or source tree, and — the point that matters most —
the artifact that gets deployed should be the exact one that was built and
tested, not a second compilation of the same source on a different machine.

The target therefore needs only:

- Ubuntu 24.04 or equivalent, PostgreSQL 16, a NATS server with JetStream, and
  altspotter running and populating its schema;
- the runtime libraries the binaries link against, which a normal system already
  has: `libpq5 libprotobuf32 libyaml-cpp0.8 libspdlog1.12 libfmt9 libssl3
  libsystemd0`.

The build host needs: `build-essential cmake ninja-build libprotobuf-dev
protobuf-compiler libspdlog-dev libfmt-dev nlohmann-json3-dev libyaml-cpp-dev
libssl-dev libsystemd-dev libpq-dev catch2`.

libpqxx, nats.c and TA-Lib are **not** taken from the distribution. They are
fetched from pinned source tarballs verified by SHA-256 and linked statically.
For libpqxx this is not a preference: distributions build it as C++17 while its
headers add a `std::source_location` parameter under C++20 and later, so linking
a C++23 program against a packaged build fails — and whether it fails depends on
which standard headers a translation unit happened to include first, so it can
succeed on one compiler version and fail on the next.

## Build

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release --output-on-failure
```

Optional flags, all off by default:

| flag | when |
|---|---|
| `-DALTTECH_NATIVE_ARCH=ON` | `-march=native`; the binary is then specific to that CPU and must not be copied elsewhere |
| `-DALTTECH_ENABLE_LTO=ON` | link-time optimization |
| `-DALTTECH_PORTABLE_RUNTIME=ON` | link libstdc++ and libgcc statically, so the binary depends on glibc alone. Needed when the build host's toolchain is newer than the target's |

## Publish

```sh
deploy/publish.sh root@host 1.0.0
```

The script stages the install, **verifies the artifact's ABI against the target**
and only then ships it. Every versioned symbol the binaries need
(`GLIBC_*`, `GLIBCXX_*`, `CXXABI_*`) must be provided by the target's
libstdc++, libc and libgcc. Without that check, shipping a binary is hopeful
rather than safe: a build host with a newer toolchain produces something that
fails at start-up, after the previous release has already been replaced. If the
check fails it names the missing symbols and tells you to build on a matching
host or turn on `ALTTECH_PORTABLE_RUNTIME`.

Staging is deliberately separate from installing. The script leaves the
release in `/tmp/alttech-deploy/stage-<id>` and prints the install command
(`vps-flat-install.sh`), which installs it as the ONE tree `/opt/alttech/` --
no `releases/`, no `current`, no backups.

First time on a host:

```sh
useradd --system --home-dir /opt/alttech --shell /usr/sbin/nologin alttech
mkdir -p /etc/alttech
```

The service user owns nothing it executes: root owns the files, the service
group only reads them; `publish.sh` sets that up.

## Database role

AltTech has its **own database**, separate from altspotter's, on the same
PostgreSQL instance. It reads altspotter's `public` tables read-only through
`postgres_fdw` foreign tables rather than by sharing a database — see
[DATABASE.md](DATABASE.md) for the full provisioning script. In outline:

```sql
-- on altspotter's database: a read-only role FDW connects back as
CREATE ROLE alttech_app LOGIN PASSWORD '<generated>';
GRANT CONNECT ON DATABASE altspotter TO alttech_app;
GRANT USAGE ON SCHEMA public TO alttech_app;
GRANT SELECT ON ALL TABLES IN SCHEMA public TO alttech_app;
ALTER DEFAULT PRIVILEGES FOR ROLE altspotter_app IN SCHEMA public
    GRANT SELECT ON TABLES TO alttech_app;

-- a separate database that alttech_app owns outright
CREATE DATABASE alttech OWNER alttech_app;
```

**The default privileges must be granted by the role that creates the tables**,
not by `postgres`. `ALTER DEFAULT PRIVILEGES` only applies to objects created by
the grantor, and altspotter's tables — including every new daily partition of
`market_candles_1m` and `market_trades_raw` — are created by `altspotter_app`.
This grants AltTech read access to those tables and partitions as they are
created.

`alttech_app` must not have write privileges on altspotter's `public` schema.
The two services being in physically separate databases makes that boundary
structural rather than a grant that could be loosened by mistake.

## NATS user

Give AltTech its own user rather than reusing altspotter's, scoped to the
namespace it owns. In `/etc/nats/nats-server.conf`:

```
authorization {
    users: [
        { user: altspotter_app, password: "…",
          permissions: { publish: ">", subscribe: ">" } },
        { user: alttech_app, password: "…", permissions: {
            publish:   { allow: ["alttech.>", "$JS.API.>", "$JS.ACK.>"] },
            subscribe: { allow: ["bar.>", "md.>", "evt.universe.>", "alttech.>", "_INBOX.>"] } } }
    ]
}
```

```sh
systemctl reload nats-server    # reload, not restart: connections are not dropped
```

## Configuration

```sh
cp /opt/alttech/share/alttech/config-examples/common.example.yaml   /etc/alttech/common.yaml
cp /opt/alttech/share/alttech/config-examples/alttech.example.yaml  /etc/alttech/alttech.yaml
cp /opt/alttech/share/alttech/config-examples/common.env.example    /etc/alttech/common.env
# fill in the DSN and NATS URL in common.env
chown root:alttech /etc/alttech/*
chmod 640 /etc/alttech/*

alttech-config-check --file /etc/alttech/common.yaml  --kind common
alttech-config-check --file /etc/alttech/alttech.yaml --kind service
```

See [CONFIGURATION.md](CONFIGURATION.md) for every key.

## Schema and topology

```sh
cp /opt/alttech/lib/systemd/system/alttech* /etc/systemd/system/
systemctl daemon-reload

systemctl start alttech-schema.service     # applies db/schema.sql, creates partitions

set -a; . /etc/alttech/common.env; set +a
alttech-nats-setup --check                 # read-only
alttech-nats-setup --apply                 # create ALTTECH_ANALYTICS and the durable
```

`--apply` is create-only: it never updates and never deletes.

## Start

```sh
systemctl enable --now alttech-maintenance.timer
systemctl enable --now alttech.target
systemctl status alttech.service
journalctl -u alttech -f
```

`alttech.target` is the stable boundary: it owns `alttech-schema.service` and
`alttech.service`. `alttech-maintenance.timer` runs hourly and keeps partitions
rolled forward, retention enforced and storage measured.

The target must be **enabled**, not only started: `alttech.service` is wanted
by the target alone. On 2026-10-18 the host was reset with the target never
enabled, AltTech stayed down and VPS1 published no signal for five days
(enabled 2026-10-23). The installer (`deploy/vps-flat-install.sh`) now fails
when the unit is not reachable from `multi-user.target`.

`alttech-schema.service` currently fails at start with
`schema_checksum_mismatch`: the database was applied from a `schema.sql` older
than the one deployed on 2026-09-25. The service validates its own module
schemas and runs; the unit fails because the tool has no migration path, and
`--recreate` would drop every AltTech table. Bringing the schema to the
deployed file is an open task.

The service reports ready to systemd only after all five modules have validated
their schemas and bound their subscriptions, so `systemctl start` returning
success means the service can actually produce output.

## Release updates

Build and publish each release from the build host with `deploy/publish.sh`.
The script stages and validates the artifact, then prints the activation
command. The schema unit applies the bundled `db/schema.sql` during service
provisioning; the service is managed by `alttech.target`.

## Resource footprint

`alttech.service` sets `MemoryMax=1536M`, `TasksMax=128` and `Nice=5`, so
AltTech yields CPU to altspotter — market-data collection must never be starved
by analytics, because data lost at the source cannot be recomputed.

The service is hardened: no capabilities, `ProtectSystem=strict`,
`PrivateTmp`, `PrivateDevices`, a restricted syscall filter, and only
`AF_UNIX`/`AF_INET`/`AF_INET6`. `WatchdogSec=120` restarts an unresponsive
process. `StartLimitIntervalSec=0` allows systemd to continue restarting it
after repeated failures.
