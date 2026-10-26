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

Macro method changes now advance stored computation availability just as
same-method value corrections do. Wire computation availability follows the
successful archive transaction; failed storage is retried without publication.
This is prospective behavior: no old feature row is rewritten to repair history,
no feature order/schema identity changes, and an earlier cutoff excludes later
replacement knowledge. The current single-row key does not reconstruct an older
method snapshot after replacement.

Do not recreate the schema or bypass a failed `alttech-schema.service` when
activating these changes. Its existing checksum/dependency problem requires
separate compatibility verification before a safe restart.
