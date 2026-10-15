# Lifecycle

```
RESEARCH → BACKTESTED → SHADOW → CANDIDATE → LIMITED → ACTIVE
                                                   ↘ DEGRADED → DISABLED → RETIRED
```

Seeds are compiled into each strategy's `default_lifecycle` and overridden per
deployment from `altstrategy.strategy_lifecycle`, loaded at startup.

## What each state means operationally

| state | runs? | candidates published? | recommended for execution? |
|---|---|---|---|
| RESEARCH / SHADOW | yes | yes, `explore_shadow` | never |
| CANDIDATE / LIMITED / ACTIVE | yes | yes, `exploit` | never |
| DEGRADED | yes | yes | never |
| DISABLED / RETIRED | no | no | no |

`recommended_for_execution` is false on every proposal: the economic verdict is
a diagnostic (an `ECON_DIAG_*` warning), the lifecycle state neither grades nor
suppresses an event, and every proposal that is not technically INVALID is
published. Whether to act on one is VPS2's decision. In production on
2026-10-15 no strategy is ACTIVE: nine run as SHADOW, `trend_pullback` as
CANDIDATE and six as RESEARCH (stored overrides plus compiled seeds).

A RESEARCH or SHADOW strategy is not idle: it produces candidates, they are
archived, and their outcomes are resolved by the same tracker. That is how a
hypothesis accumulates the record that would justify promotion.

## What a seed is not

**A lifecycle seed is a starting position, not a claim about edge.** Two
strategies are seeded ACTIVE (`momentum_continuation`, `trend_pullback`), and
the evidence in `docs/STRATEGY_RESEARCH.md` argues against both:
`momentum_continuation` is negative gross in all four walk-forward folds and is
flagged `STRUCTURAL_BREAK` by the live drift scan over 10,623 samples. The seeds
predate any measurement; they were an author's expectation.

Nothing in the current evidence supports promoting any strategy toward
execution, and the honest response to a seed contradicted by measurement is to
demote it — not to tune the strategy until the measurement agrees.

## Drift and demotion

`strategy_drift_snapshots` records per-strategy outcome drift; the scan runs
continuously and can transition a strategy to DEGRADED. Drift is judged against
its own uncertainty rather than its raw magnitude, so a strategy with few
samples does not get demoted for noise.

## Adding a strategy

Implement `IStrategy` in `src/engine/strategies/*.cpp`, register it with
`ALTSTRATEGY_V2_REGISTER_STRATEGY`, seed it RESEARCH, document the hypothesis
and its falsification in `docs/STRATEGY_SOURCES.md`, and let evidence — replay,
walk-forward stability, then live outcomes — carry it forward. The pure-logic
tests in `tests/unit/test_engine_strategies.cpp` cover LONG/SHORT/no-setup,
eligibility and determinism; they prove the code does what it says, which is not
the same as the hypothesis being true.
