# Horizons and the 48-hour ceiling

## The ceiling is a refusal, not a clamp

`kMaxHoldingMs = 48h` (`decision_types.hpp`) is the absolute holding ceiling for
every strategy, candidate, plan and signal in the system.

Candidates whose `expected_horizon_ms` exceeds the ceiling are **rejected**.
`clamp_holding(declared, horizon_ms)` also bounds each plan by the holding class
declared by its strategy.

The ceiling bounds the maximum lifetime of any candidate, plan or signal. It is
a safety limit rather than a target horizon.

## Declared horizons

| holding class | plan bound | strategies |
|---|---|---|
| SCALP | 30 min | book_imbalance_pressure (20 m), spread_dislocation_fade (15 m), cvd_absorption_reversal (45 m), venue_dislocation_convergence (30 m) |
| INTRADAY | 8 h | the trend, breakout, vwap and mean-reversion catalogue (90 m – 4 h) |
| EXTENDED_INTRADAY | 48 h | funding_basis_convergence (8 h), funding_oi_crowding_unwind (3–8 h) |

The longest horizon any strategy declares is **8 hours**, six times inside the
ceiling. `funding_basis_convergence` declares 8 h because the funding settlement
clock — not a price target's arrival — is what pays its thesis; it is the only
strategy in the catalogue whose horizon is set by a contractual event rather
than by an expected move.

## Where the ceiling is enforced

1. `StrategyEngine` — refuses a candidate declaring more than the ceiling.
2. `generate_plans` — `clamp_holding` bounds each variant by its declared class.
3. `build_proposal` — `max_holding_time_ms` and `time_stop_at_ms` carried onto
   the proposal.
4. Regression tests pin every layer, and
   `tests/unit/test_engine_strategies.cpp` asserts that every registered
   strategy declares a horizon inside the ceiling.
