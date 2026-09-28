# Integration

```text
AltStrategy → AltSignalQuality → AltSignalGateway → authenticated WebSocket
```

AltSignalQuality is the validity and publication stage between proposal
generation and durable external delivery. It checks the upstream contract and
freshness/holding-time constraints, records its evaluation and forwards
accepted proposals. It does not create or modify the trade plan.

## Input

The evaluator consumes `altstrategy.v1.Envelope` messages carrying a
`TradeSignalProposal` from `ALTSTRATEGY_DECISIONS` on
`signal.proposed.>`. Its durable is `signalquality-candidates`. The upstream
schema is vendored under `proto/vendor/` and the wire fields are validated
before processing.

Point-in-time context and proposal metadata are read from AltStrategy's
PostgreSQL schema. AltSignalQuality has read access to those tables and owns
only its `altsignalquality` schema.

## Output

| subject | message | publication |
|---|---|---|
| `signalquality.final.<symbol>.v1` | `altsignalquality.v1.FinalTradeSignal` | Accepted proposals |
| `signalquality.evaluation.<symbol>.v1` | `SignalQualityEvaluation` | Every evaluated proposal |
| `signalquality.model.<event>.v1` | `ModelLifecycleEvent` | Model registry transitions |

`FinalTradeSignal` carries the original `TradeSignalProposal` unchanged, a
deterministic `final_signal_id`, evaluation metadata and producer version. The
quality score may be absent; when present it is diagnostic and does not change
the live decision.

## Downstream boundary

AltSignalGateway consumes final signals from `ALTSIGNALQUALITY_SIGNALS`,
journals them and delivers them to clients using its authenticated WebSocket
protocol. External consumers should integrate with Gateway rather than connect
to AltSignalQuality's NATS or PostgreSQL resources. See the
[AltSignalGateway consumer guide](../../altsignalgateway/docs/CONSUMER_GUIDE.md)
and [signal contract](../../altsignalgateway/docs/SIGNAL_CONTRACT.md).

Consumers should treat `final_signal_id` as the idempotency key and honor the
proposal's validity and entry-fill deadlines. The Gateway protocol defines
delivery acknowledgements and resume semantics.

## Service dependencies

The evaluator requires PostgreSQL access to its own schema and the AltStrategy
tables it reads. It also requires the AltStrategy input stream and its owned
output stream, which `nats-setup` provisions and validates. The JetStream
durable resumes at its stored cursor across service restarts.
