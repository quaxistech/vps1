# Operations

## Service lifecycle

The service and its schema unit are managed by systemd. The maintenance timer
handles partition creation, retention, and storage measurements.

```sh
systemctl status alttech.service
systemctl start alttech.target
systemctl stop alttech.target
journalctl -u alttech.service -f
```

## Inspection

```sh
alttech-analytics cross-venue-inspect [--pair ID]
alttech-analytics alignment-inspect
alttech-analytics storage-inspect
```

`cross-venue-inspect` reports quality, consensus, regimes, and zones.
`alignment-inspect` reports alignment state. Storage policy and measured
capacity are described in [Storage](STORAGE.md).

## Configuration and interfaces

Configuration examples are under `config/`. Validate a service file with
`alttech-config-check --file PATH --kind service` and a common file with
`--kind common`. Schema and NATS provisioning are managed by the
`alttech-schema.service` unit and `alttech-nats-setup`; see [Deployment](DEPLOYMENT.md).
