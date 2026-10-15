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
`altstrategy-nats-init` provisions the owned stream and durable consumers. In
production (2026-10-15) the stream sits at its 256 MiB `max_bytes` with
`discard: old`, so it holds about 17 days, not the configured 30.

## Publication: the transactional outbox

`tradeproposal.*`, `signal.proposed.*` and `signal.market_outcome.*` are never
published by the decision path itself. The transaction that archives a
decision (candidate, plans, proposals) or a market outcome also writes its
messages to `publication_outbox`; a decision whose archive fails is published
nowhere. One `OutboxPublisher` thread then sends each row with JetStream
`js_publish`, `Nats-Msg-Id = <kind>:<id>`, and marks it sent only on the
stream's ack (its stream sequence recorded). A refused publish backs off
exponentially (1 s doubling to a 60 s cap) and survives restarts. A row unsent
after `publication.max_age_hours` (24) is dead-lettered: kept with its payload
and last error, counted in the publisher's per-minute report
(`dead_lettered_total`), never purged, and sent again under its original id
only when an operator requeues it (`db/operator/outbox_requeue_dead_lettered.sql`).
Retention reclaims acknowledged rows only.

The stream drops a repeated `Nats-Msg-Id` inside its duplicate window, which
`altstrategy-nats-init` provisions at 24 h -- the outbox's retry horizon -- so a
publish whose ack was lost is stored once however late it is retried. (The
production stream keeps its 2-minute window until `altstrategy-nats-init` runs
there.) Behind that, both consumers are idempotent by signal id:
AltSignalQuality claims each proposal once and re-publishes a final signal only
under the same id; the Gateway's journal is keyed by the signal id and gives a
duplicate no new sequence. A redelivery never reaches VPS2 as a new signal.

Candidates, ensembles and rankings remain best-effort Core publishes of an
archived decision, without a `Nats-Msg-Id`: counted when refused, never
retried, and published again when the same context is decided again. They are
observability on `strategy.*`; nothing downstream reads them as signals. A
ranking that could not be archived is not published, and the proposals it
ranked name no ranking (`ranking_id` empty) rather than a missing one.

Strategy drift, lifecycle transitions, plan variants, calibration records,
counterfactuals, and attribution are stored in PostgreSQL. They are not NATS
subjects.

AltStrategy publishes proposals to the internal quality pipeline. Final signal
delivery to an external client is provided by AltSignalGateway; an external
consumer does not subscribe directly to AltStrategy's stream.
