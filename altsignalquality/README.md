# AltSignalQuality

AltSignalQuality validates AltStrategy's signal proposals, records an auditable
decision and publishes accepted proposals as final signals. It is a quality and
contract boundary; it does not make the market or execution decision, change a
trade plan, or execute orders.

```text
AltStrategy → AltSignalQuality → AltSignalGateway → external consumer
```

## Responsibilities

- Consume `signal.proposed.>` from AltStrategy's JetStream stream.
- Check the proposal contract, configured freshness limit and the maximum
  holding-time invariant.
- Preserve the upstream proposal and add AltSignalQuality's evaluation
  metadata; accepted signals are published to the service-owned stream.
- Store decisions and point-in-time feature vectors in the owned
  `altsignalquality` PostgreSQL schema.
- Optionally train a calibrated signal-quality model. Its score is diagnostic;
  the learned score does not gate publication. Training is disabled by default.

The default policy forwards valid signals. `REJECT_ALL` is available for a
deliberate freeze. The service never infers an execution recommendation from a
quality score.

## Temporal integrity and recovery

Evaluation stores the exact point-in-time feature basis before publication.
As-of priors use only matured outcomes available by the signal cutoff; live
training reuses the evaluation vector and backfill uses Strategy's immutable
archive. A model artifact must match its checksum and feature schema. Missing
models produce an absent diagnostic score, not fabricated confidence or a
publication veto. [Cold-start policy](docs/COLD_START.md) explains why the
historic `policy.cold_start` name applies in every model state.

Pending accepted rows can be republished after restart with deterministic
message identity. Dataset watermarks and retention policies belong to the
`altsignalquality` schema; no worker may mutate Strategy's proposals or context.
[Architecture](docs/ARCHITECTURE.md), [leakage rules](docs/LEAKAGE.md) and
[operations](docs/OPERATIONS.md) define storage, dependency and recovery behavior.

## Commands

```sh
altsignalquality run
altsignalquality train
altsignalquality validate
altsignalquality model-status
altsignalquality dataset-build
altsignalquality nats-setup
```

Operational settings are in `config/altsignalquality.example.yaml`; secrets are
expanded from the service environment. Build presets and release scripts are in
the repository root and `scripts/`.

## Documentation

Start at [docs/README.md](docs/README.md) for architecture, contracts, model
lifecycle, configuration and routine operations.
