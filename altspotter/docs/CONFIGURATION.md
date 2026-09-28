# Configuration

The example YAML files are the reference for names, structure, and example values:

- [altspotter.example.yaml](../config/altspotter.example.yaml) contains venue, universe, collection, storage, and retention settings.
- [common.example.yaml](../config/common.example.yaml) contains shared service settings and environment-variable references.
- [symbols.yaml](../config/symbols.yaml) lists configured base assets.

Production copies belong under <code>/etc/altspotter/</code>. The files are:

| File | Purpose |
|---|---|
| <code>altspotter.yaml</code> | AltSpotter service configuration |
| <code>common.yaml</code> | PostgreSQL, NATS, logging, timezone, and instance settings |
| <code>symbols.yaml</code> | configured base-asset universe |
| <code>common.env</code> | values used to expand environment-variable references |

The example common configuration references <code>ALTSPOTTER_ENVIRONMENT</code>, <code>ALTSPOTTER_LOG_LEVEL</code>, <code>ALTSPOTTER_INSTANCE_ID</code>, <code>ALTSPOTTER_NATS_URL</code>, and <code>ALTSPOTTER_POSTGRES_DSN</code>. Keep credentials outside the repository and provide them through the service environment.

Validate a configuration with:

    altspotter config-check --config /etc/altspotter/altspotter.yaml --common /etc/altspotter/common.yaml

## Configuration sections

The service configuration covers:

- enabled venues and the global observation universe;
- instrument catalog synchronization;
- live venue connections and market-data collection;
- REST history bootstrap, reconciliation, and repair;
- database writers and time-frame aggregation;
- data-integrity checks;
- storage limits, retention windows, and sampling.

The example YAML is annotated and contains the complete supported configuration shape. This document describes the relationships that affect behavior; it does not duplicate every key and default.

## Constraints

Configuration validation enforces the service’s cross-field invariants:

- supported venue and market combinations must be selected;
- an instrument is eligible only when active on all enabled venues;
- configured history depth must fit within the corresponding retention window;
- the projected retention footprint must fit within the storage budget’s cleanup threshold;
- collection timestamps use UTC;
- shared order-book sampling settings apply across venues.

See [Retention](RETENTION.md), [Storage budget](STORAGE_BUDGET.md), and [Global universe](GLOBAL_UNIVERSE.md) for the associated policies.
