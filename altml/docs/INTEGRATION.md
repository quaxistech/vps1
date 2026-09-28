# AltML integration contract

AltML is an independent service. Applications connect to it over NATS
JetStream using the `altml.v1` protobuf schema in [../proto/](../proto/) —
nothing links against AltML code, and AltML knows nothing about its
consumers.

## Connecting an application

1. Compile `altml.v1` from `proto/` (`Envelope` with a `PredictionV1`
   payload). Consumers in any language only need these small files.
2. Create your own durable consumer on the AltML-owned output stream
   (default `ALTML_ANALYTICS`, subjects `prediction.>`). Never attach to
   another application's durable and never provision the stream yourself.
3. Predictions arrive on `prediction.<exchange>.<symbol>.1m.<model_version>`
   (default prefix `prediction.bitget`). Deduplicate by `message_id`
   (`pred:<sym>:1m:<bar_open_ms>:r<rev>:<model_id>`).
4. Model lifecycle events (`evt.model.registered`, `evt.model.transition`)
   are published on the AltML-owned control stream (default `ALTML_CONTROL`).
   Envelope `source_service` is `altml`.

All stream/subject names above are the defaults of the `project:` adapter in
`/etc/altml/altml.yaml`; a deployment may rename them per project.

## Input side (feature producer)

AltML consumes `alttech.v1.FeatureVector` messages from the externally owned
`ALTTECH_ANALYTICS` stream (`alttech.feature.venue.bitget.>`) through its own
durable (`inference-features`). The producer contract is vendored in
`proto/alttech/` and must mirror `altml.v1.FeatureVectorV1` field-for-field
(guarded by `tests/unit/test_wire_contract.cpp`). See [NATS.md](NATS.md) for
why the subject filter is scoped to the primary venue only.

## PostgreSQL

AltML reads AltSpotter's and AltTech's tables **read-only**, through
`postgres_fdw` (see [DATABASE.md](DATABASE.md)), and owns its own `altml`
schema. Consumers of AltML's predictions should rely on the NATS contract
above, not on joining AltML's tables directly — the registry schema carries
no external `project_id` and its `ACTIVE`-model uniqueness is global per
objective.

## Adding another project/exchange

The `project:` adapter makes every external name configurable per instance:
exchange code, source stream/filter/durable, prediction subject prefix,
control stream/subjects, model root. Today only `exchange: bitget` is
implemented; connecting a second exchange means deploying an isolated AltML
instance with its own config, database and NATS namespace — the schema is
still single-project.
