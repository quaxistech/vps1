# Deployment

AltStrategy is built on the build host and installed as a native systemd
service. The target host needs the runtime libraries and PostgreSQL/NATS access,
not the source tree or build toolchain.

## Build and publish

```sh
cmake --preset release
cmake --build --preset release
scripts/deploy.sh
```

The deploy script stages and verifies the release, installs it under
`/opt/altstrategy/`, and leaves a stopped service stopped. It does not apply
database migrations.

## Host setup

Provision the service account and data directory with `scripts/provision.sh`.
Install the database schema by applying the ordered files under
`db/migrations/` to the AltStrategy-owned schema. Configure the service under
`/etc/altstrategy/`, then provision `ALTSTRATEGY_DECISIONS` with
`altstrategy-nats-init`.

The service reads PostgreSQL and NATS credentials from its environment file.
Credentials stay outside the source and release archive.

## Start and update

```sh
systemctl enable --now altstrategy.service
```

For a release that changes the database schema, apply the corresponding
migration before starting the new binary. The service runs as one systemd unit
and stores its runtime archive and exports under `/var/lib/altstrategy/`.
