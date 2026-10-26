# Operations

## Service lifecycle

The normal service runs as one supervised process. Evaluator, dataset,
trainer and model-registry components share its lifecycle; training remains
disabled unless enabled in configuration.

```sh
systemctl enable --now altsignalquality.service
systemctl status altsignalquality.service
systemctl restart altsignalquality.service
```

Run directly with:

```sh
altsignalquality run --config /etc/altsignalquality/altsignalquality.yaml
```

## Initial setup

1. Install a release using `scripts/deploy.sh`.
2. Set service environment values in the configured environment file.
3. Apply the database schema with `scripts/db-bootstrap.sh`.
4. Provision the JetStream stream and durable:

   ```sh
   altsignalquality nats-setup --config /etc/altsignalquality/altsignalquality.yaml
   ```

5. Start `altsignalquality.service`.

Database migrations and deployment are separate. The service reads AltStrategy
tables and market outcomes; its database role owns only the
`altsignalquality` schema and reads upstream tables.

## Routine commands

```sh
altsignalquality validate --config /etc/altsignalquality/altsignalquality.yaml
altsignalquality model-status --config /etc/altsignalquality/altsignalquality.yaml
altsignalquality dataset-build --config /etc/altsignalquality/altsignalquality.yaml
```

`validate` audits contracts, dataset and split invariants. `model-status`
reports registry and dataset state. `dataset-build` runs one materialization
pass. `train` runs one training cycle when training is enabled.

The example configuration is `config/altsignalquality.example.yaml`.
Environment substitutions are required for deployment-specific values; secret
values are supplied through the service environment.

## Release updates

Build and package from the repository with `scripts/build-release.sh`, then
install the artifact with `scripts/deploy.sh`. Restart the service after an
update. When a release changes the feature schema, models trained under another
schema remain incompatible until a matching model is registered.

## Frozen publication evidence

Accepted first claims now persist the complete final publication bytes and an
input digest in the same transaction as the evaluation. Initial publish,
redelivery and restart use those frozen bytes; model/context/policy changes
cannot replace them. The final envelope's publication-intent timestamp remains
separate from evaluation time and the database's successful-publication time.
Conflicting or unverifiable same-ID inputs are refused through the existing
terminal-input path. No historical decision is rewritten.

The additive 0002 migration is required before the new writer. Use the packaged
`apply-additive-migrations.sh` with explicit `PGDATABASE` and the existing owner
connection environment. It verifies migration checksums and bounds DDL locks;
it does not reset roles, grants or retention. Avoid the full bootstrap script.
Before switching versions, verify there are no legacy pending accepted rows
without frozen publication evidence; recheck after the old daemon has stopped
cleanly as part of the bounded service restart. If any exist, retain the previous
release and investigate without guessing the original evaluation.

Executable rollback preserves the nullable columns and ledger. Rollback to a
legacy publisher requires an empty pending publication set after graceful stop;
that publisher does not understand frozen publication bytes. Never downgrade a
pending accepted decision into a newly computed evaluation.
