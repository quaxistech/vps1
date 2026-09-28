# Operations

## Initial setup

Release installation, database initialization and client configuration are
separate steps. On the service host:

1. Install a release with `scripts/deploy.sh`.
2. Create the service account and directories with `provision.sh`.
3. Apply the database migration with `db-bootstrap.sh` as the database
   administrator.
4. Create the NATS account with `nats-user.sh`.
5. Install TLS material and set connection values in
   `/etc/altsignalgateway/common.env` and client identities in
   `/etc/altsignalgateway/clients.yaml`.
6. Validate configuration and provision JetStream resources:

   ```sh
   altsignalgateway validate --config /etc/altsignalgateway/altsignalgateway.yaml
   altsignalgateway nats-setup --config /etc/altsignalgateway/altsignalgateway.yaml
   ```

7. Enable the service:

   ```sh
   systemctl enable --now altsignalgateway.service
   ```

The deployment script installs a release; it does not apply migrations,
provision clients or expose a network port. See [SECURITY.md](SECURITY.md) for
the network and credential boundary.

## Routine service management

```sh
systemctl status altsignalgateway.service
systemctl restart altsignalgateway.service
systemctl stop altsignalgateway.service
altsignalgateway status --config /etc/altsignalgateway/altsignalgateway.yaml
```

The `status` command reports journal head and backlog, client cursors, session
state and storage size.

## Journal inspection and integrity

```sh
altsignalgateway inspect --sequence 101
altsignalgateway inspect --sequence 101 --source
altsignalgateway verify --limit 5000
altsignalgateway selftest --config /etc/altsignalgateway/altsignalgateway.yaml
```

Inspection prints the canonical payload or original upstream envelope as JSON.
`verify` checks journal hashes and deterministic serialization. `selftest`
checks database invariants inside a rolled-back transaction.

Retention runs according to `journal.retention_interval_sec` and the policies
in `altsignalgateway.retention_policies`. Review its recorded runs with:

```sql
SELECT started_at, rows_deleted, detail
FROM altsignalgateway.retention_runs
ORDER BY id DESC LIMIT 5;
```

## Releases

Build and package with `scripts/build-release.sh`, install with
`scripts/deploy.sh`, then restart the systemd service. Schema migrations remain
a separate operation. The durable journal and client cursors persist across
service releases.
