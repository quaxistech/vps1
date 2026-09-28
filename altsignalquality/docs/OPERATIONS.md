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
