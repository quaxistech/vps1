# Operations

## Service

AltML runs as one systemd service. Its training schedule is managed inside the
process; it has no separate training timer or one-shot service.

```sh
systemctl status altml.service
systemctl start altml.service
systemctl stop altml.service
journalctl -u altml.service -f
```

The service configuration and environment file are under `/etc/altml/`.
Model artifacts are stored under `/var/lib/altml/models/`.

## State and manual commands

`systemctl status altml.service` reports service readiness. `altml train` runs
one training cycle and `altml replay` reproduces stored predictions. Routine
training is scheduled automatically; see [Autonomous lifecycle](AUTONOMOUS_LIFECYCLE.md).
`altml model-validate` checks a model artifact against recorded predictions.

`altml nats-setup` provisions AltML-owned JetStream resources. The service also
performs this setup during startup.

## Deployment and configuration

Build and install the service with the scripts under `deploy/` and `scripts/`.
Database roles, read-only source access, NATS permissions, and configuration
files are described in [Database](DATABASE.md), [NATS](NATS.md),
[Configuration](CONFIGURATION.md), and the repository [README](../README.md).

## Original live-input durability

The inference consumer commits the original AltTech envelope in
`feature_vectors.original_live_payload` before cache/model processing and source
ACK. This also applies without a loaded model. Persistence failures use the
existing delayed NAK path; monitor consumer backlog and archive errors rather
than clearing the durable. The existing archive values remain first-wins when
backfill arrived first; the nullable payload separately retains the original
live-observed vector. A conflicting original vector under the same archive key
is an error, never successful persistence.

Apply `0001_original_live_payload.sql` before activating this writer, using the
packaged `apply-additive-migrations.sh` with explicit `PGDATABASE` and the
existing owner connection environment. The runner checks its separate additive
migration ledger and bounds DDL locks/statements. Do not rerun full bootstrap or
change the historical `schema.sql` checksum for this extension. Check partition
count, free space and storage growth first: original envelope bytes add storage
within the existing partition retention. A lock timeout is a failed migration,
not permission to remove the bound during live processing.

Executable rollback leaves the nullable column and additive ledger in place;
the old binary ignores them. It also restores the old archive-before-ACK
limitation, so a restored process does not establish original-input durability.
Retain already captured bytes and consumer state; do not reconstruct missing
historical live inputs from later feature revisions.

Credentials belong in `/etc/altml/common.env`. Keep them out of source files,
command arguments, and logs.
