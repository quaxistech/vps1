# NATS contracts

## Input

| stream | subject | owner | use |
|---|---|---|---|
| `ALTSTRATEGY_DECISIONS` | `signal.proposed.>` | AltStrategy | Candidate proposals consumed by durable `signalquality-candidates` |

AltSignalQuality validates the producer-owned stream and creates its durable
consumer. It does not create or reconfigure the input stream.

## Output

| stream | subject | message | use |
|---|---|---|---|
| `ALTSIGNALQUALITY_SIGNALS` | `signalquality.final.<symbol>.v1` | `FinalTradeSignal` | Accepted proposals; consumed by AltSignalGateway |
| `ALTSIGNALQUALITY_SIGNALS` | `signalquality.evaluation.<symbol>.v1` | `SignalQualityEvaluation` | Evaluation audit for accepted and rejected proposals |
| `ALTSIGNALQUALITY_SIGNALS` | `signalquality.model.<event>.v1` | `ModelLifecycleEvent` | Optional trainer and registry lifecycle events |

The output stream and durable are provisioned by `nats-setup`; the service also
validates/provisions its owned JetStream resources at startup. Output subjects
use the `signalquality.` namespace, separate from AltStrategy's `signal.`
namespace.

## Delivery and idempotency

The candidate consumer uses explicit acknowledgements. Evaluation rows and
published identifiers are deterministic in the proposal identity. Accepted
signals are persisted before publication; pending publication can be retried
with the same message identifier. JetStream de-duplicates messages by that
identifier within the configured duplicate window.

Accepted signals and evaluations are separate publications. The evaluation
stream is an audit surface; it does not control the final-signal publication.

## Provisioning

```sh
altsignalquality nats-setup --config /etc/altsignalquality/altsignalquality.yaml
```

Configuration source: `wiring.input` and `wiring.output` in
`config/altsignalquality.example.yaml`.
