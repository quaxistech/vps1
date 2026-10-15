# Architecture

AltStrategy is one binary (`altstrategy-service`), one process, one systemd unit.
It is a **self-contained VPS1 adaptive decision engine**: everything it needs to
generate signals, score their outcomes, adapt, and produce training data lives
inside this one process and the AltStrategy-owned `altstrategy` PostgreSQL
schema. An external execution consumer (risk / sizing / execution) is a downstream *consumer*, never a
dependency.

## Event-driven pipeline

The engine is triggered by upstream **events**:

```
AltTech analytics component (feature / regime / structure / quality)
AltSpotter universe change
AltML PredictionV1
        │
        ▼
context_assembler ── coherent, versioned DecisionContext for a pair? ──► run_for_context
```

`run_for_context` (`src/engine/io/decision_engine.cpp`) runs synchronously on the
delivery path:

1. `strategy_engine` runs every **eligible** strategy → all candidates.
2. `adaptive_strategy_allocator` attaches the Bayesian-shrunk posterior + weight.
3. the context is archived first -- a context that cannot be archived decides
   nothing -- and every candidate goes on (weak candidates are never hidden).
4. `strategy_ensemble` computes direction / agreement / family independence.
5. `strategy_ranking` runs the **champion** and the **challenger** (shadow).
6. the recommended candidate → three `trade_plan` geometries.
7. each geometry → `altml_candidate_evaluator` (bounded; degrades to a
   deterministic estimate on timeout) → `proposal_publisher`.
8. one transaction per candidate archives it, its plans, its proposals and the
   outbox messages of the non-INVALID ones; the outbox publisher sends them
   after the commit and only marks what the stream acknowledged
   ([NATS.md](NATS.md)).

Stage latencies (`context_to_candidates`, `candidates_to_ranking`,
`ranking_to_plans`, `plan_to_altml`, `altml_to_proposal`, `context_to_proposal`)
are sampled and reported as p50/p90/p99 every 60 s.

## Maintenance scheduler

One internal thread (`maintenance_scheduler`) runs the periodic self-contained
jobs — outcome maturation, stats/posterior refresh, drift scan, parameter
calibration, training-example materialization, snapshot export, retention. None
of these jobs depend on a downstream consumer.

## Failure isolation

- Fatal: schema/contract mismatch, advisory-lock loss, DB corruption, invalid
  config → process exits non-zero.
- Degraded (process stays up, live pipeline continues): AltML unavailable, the external consumer
  unavailable, one strategy exception, outcome backlog, snapshot-export failure.
  AltML timeouts yield an `ML_UNAVAILABLE` warning + deterministic proposal.
- NATS unavailable: decisions are still archived; their messages wait in the
  outbox and go out when the stream is back. PostgreSQL unavailable: a decision
  is retried for a dropped connection, otherwise logged (`decision_not_archived`)
  and published nowhere. A ranking that cannot be archived is retried the same
  way; failing that, the proposals it ranked are published naming no ranking
  (`rankings_not_archived` in `decision_output_failures`).
- A schema older than the one this binary writes (`schema_not_migrated`):
  the service refuses to start rather than run and archive nothing.

## Code layout

```
proto/            decision.proto (altstrategy.v1) + vendored altml_forecast / alttech_analytics
src/core/         common · time · logging · analytics_contracts
src/infra/        config · messaging (NATS) · persistence (PostgreSQL)
src/engine/       pure decision logic (catalogue, ensemble, allocator, ranking,
                  plan, proposal, outcomes, attribution, adapt) — no IO, fully tested
src/engine/io/    wire_codec · context_assembler · decision_store · altml_evaluator ·
                  market_reader · snapshot_exporter · maintenance_scheduler ·
                  decision_engine (orchestrator)
src/app/          composition root (Application)
db/migrations/    altstrategy schema (single owned migration)
tests/            engine unit tests + full-path integration test + IO tests
```

The pure `src/engine/` layer links only `altstrategy_common`/`_time` and is unit-
tested in isolation; proto and PostgreSQL appear only under `src/engine/io/`.
