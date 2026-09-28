# Architecture

AltSignalQuality is one supervised process with four components. It validates
AltStrategy proposals, records evaluation state and publishes accepted signals
for AltSignalGateway. It does not alter the trade plan or make an execution
decision.

## Components

```text
AltStrategy proposals (JetStream)
             │
             ▼
          evaluator ─────► FINAL signal (JetStream)
             │
             └───────────► signal_evaluations
                              │
                ┌─────────────┴─────────────┐
                ▼                           ▼
             dataset                     trainer
                │                           │
      training_examples               model registry
      outcome_priors                 model artifacts
```

- **Evaluator:** consumes the configured durable, validates proposal shape,
  freshness and holding-time limits, assembles the point-in-time feature
  vector, records the decision and publishes accepted signals. A model score,
  when present, is diagnostic and does not gate publication.
- **Dataset:** builds as-of priors and immutable training examples. Live
  examples reuse the vector recorded during evaluation; backfilled examples
  use AltStrategy's point-in-time archive.
- **Trainer:** optional. It fits and calibrates a LightGBM quality model using
  walk-forward validation and the configured promotion gates. Training is
  disabled in the example configuration.
- **Model registry:** observes model lifecycle state and publishes model events
  on the configured service-owned stream.

## Persistence and idempotency

AltSignalQuality owns the `altsignalquality` PostgreSQL schema. It reads
AltStrategy's proposal, context, warning and outcome tables without modifying
them. Proposal identity anchors evaluation rows; deterministic publication IDs
allow a pending accepted signal to be republished after a restart.

Dataset jobs use persistent watermarks. Labels only become eligible after
their outcomes mature. Retention is controlled by the owned
`retention_policies` table.

## Point-in-time inputs

The feature vector has one ordered, versioned contract. Each input is marked
available at signal time, and missing values remain explicit. Training and
inference use the same assembler. As-of priors include only matured outcomes
available by the signal timestamp.

See [FEATURE_CONTRACT.md](FEATURE_CONTRACT.md), [DATABASE.md](DATABASE.md) and
[LEAKAGE.md](LEAKAGE.md) for the detailed contracts.
