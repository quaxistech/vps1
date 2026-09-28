# NATS contracts

## Inputs

AltStrategy consumes upstream data through Core NATS subscriptions:

| Subject | Producer | Use |
|---|---|---|
| `evt.universe.>` | AltSpotter | Instrument eligibility |
| `alttech.feature.>` · `alttech.microfeature.>` | AltTech | Ordered feature vectors |
| `alttech.regime.>` | AltTech | Venue and cross-venue regime context |
| `alttech.structure.>` | AltTech | Projected and consensus zones |
| `alttech.quality.>` | AltTech | Cross-venue quality |
| `prediction.bitget.<symbol>.<tf>.v1` | AltML | Market forecast |
| `signal.execution_feedback.>` | Execution integration | Optional execution outcomes |

AltStrategy owns its subscriptions and does not create upstream streams. Message
schemas are the versioned protobuf contracts vendored under `proto/`.

## Outputs

AltStrategy owns the `ALTSTRATEGY_DECISIONS` JetStream stream. Its subjects are:

```text
strategy.candidate.<symbol>.<strategy>.v1
strategy.ensemble.<symbol>.v1
strategy.ranking.<symbol>.v1
tradeproposal.<symbol>.v1
signal.proposed.<symbol>.v1
signal.market_outcome.<symbol>.v1
archive.snapshot.created.v1
```

The stream's storage, retention, and size limits are configured by the service.
`altstrategy-nats-init` provisions the owned stream and durable consumers.

Strategy drift, lifecycle transitions, plan variants, calibration records,
counterfactuals, and attribution are stored in PostgreSQL. They are not NATS
subjects.

AltStrategy publishes proposals to the internal quality pipeline. Final signal
delivery to an external client is provided by AltSignalGateway; an external
consumer does not subscribe directly to AltStrategy's stream.
