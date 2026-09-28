# NATS contracts

AltML owns its output and control streams; consumers only ever create their
own durables on them. Provisioning runs inline at `altml run` startup
(idempotent) and via `altml nats-setup` for manual/CI use.

## Input (consumed, never created)

```
alttech.feature.venue.bitget.<symbol>.1m.core.v1
```

on the externally owned `ALTTECH_ANALYTICS` stream, through AltML's own
durable `inference-features`. This filter is deliberately scoped to the
primary-venue ("core") Bitget feature vector: AltTech also publishes
reference-venue (Binance/OKX/Bybit) and cross-venue vectors under the same
`alttech.feature.>` namespace with a **different** payload shape
(`venue_feature_vector` / `cross_venue_feature_vector`, not `feature_vector`)
that this service does not decode — subscribing to the broader
`alttech.feature.>` wildcard would receive those too and reject every one of
them as an unsupported payload.

## Output (owned by AltML)

| Stream | Subjects | Contents |
|---|---|---|
| `ALTML_ANALYTICS` | `prediction.bitget.<symbol>.1m.<model_version>` | `altml.v1.PredictionV1` |
| `ALTML_CONTROL` | `ml.>`, `evt.model.>`, `cmd.model.>` | model lifecycle events |

Predictions are deduplicated by `message_id`
(`pred:<sym>:1m:<bar_open_ms>:r<rev>:<model_id>`); model lifecycle events by
`model:<id>:<status>`. Both streams keep 30 days and are capped (1 GiB /
512 MiB) — see `src/altml/core/nats_provision.cpp`.

## Ownership

| Resource | Owner |
|---|---|
| `ALTTECH_ANALYTICS` (input) | AltTech — validated by AltML, never created |
| `ALTTECH_ANALYTICS/inference-features` durable | AltML |
| `ALTML_ANALYTICS` (`prediction.*`) | AltML |
| `ALTML_CONTROL` (`ml.>`, `evt.model.>`, `cmd.model.>`) | AltML |
| consumer durables on `ALTML_ANALYTICS` | each consuming application |

Stream/subject names above are the `project:` adapter defaults in
`altml.yaml` and are configurable per instance — see
[INTEGRATION.md](INTEGRATION.md).

## Shared JetStream storage budget

AltML's NATS user shares one JetStream account (and its `max_file_store`
ceiling) with AltSpotter and AltTech on the same server. Each stream's
`max_bytes` is reserved against that ceiling at creation time, regardless of
how full it actually is — leave enough headroom in the account's storage
limit before provisioning AltML's streams on a host that already runs the
other two services.
