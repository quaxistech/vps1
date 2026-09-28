# Data and model lineage

A prediction or signal should be traceable to its source market observations, feature definitions, labels, and model version.

## Lineage

    venue observations
        └─ AltSpotter normalized records
            └─ time-aligned features
                ├─ training labels and dataset snapshot
                │   └─ evaluated model version
                │       └─ prediction
                └─ decision context at T0
                    └─ market setup and outcome
                        └─ quality evaluation and published signal

## Provenance to retain

| Artifact | Provenance |
|---|---|
| Market observation | Venue, canonical instrument, event time, source, revision |
| Feature | Input interval and revisions, feature schema and engine version |
| Label | Anchor interval, horizon, label definition version, validity state |
| Dataset | Included time range, source revisions, feature and label versions |
| Model | Training dataset, model artifact, evaluation and promotion state |
| Prediction | Model version, inference time, input feature versions, quality state |
| Decision context | T0, context revision, input identities and availability |
| Published signal | Signal identity, source decision, quality evaluation, schema version |

Version stamps are part of the data contract. Consumers must not treat records with different feature, label, cost, or model versions as equivalent without an explicit compatibility rule.

The point-in-time constraints are documented in [Temporal model](TEMPORAL_MODEL.md) and [Point-in-time data rules](DATA_LEAKAGE_MODEL.md).
