# Market outcomes

AltStrategy computes proposal outcomes from AltSpotter's durable market history.
This records what the market did after a proposal; it is separate from any
execution result.

## Path reconstruction

`MarketReader::read_price_path` reads the window from the proposal to one
minute past its time stop:

1. one-second buckets of raw trades, **only** when they provably cover the
   window: every minute has its 1m candle, and every minute whose candle
   traded has trades. A second without a bucket is then a second without a
   trade, not a hole;
2. otherwise the 1m candles, every missing minute recorded as a gap.

The path's source, bar length and gap count are recorded with the outcome.

## Evaluator version 2 (`conservative-1`)

`reconstruct_outcome_v2` (`src/engine/outcomes.cpp`) never takes the reading
that flatters the plan:

| case | rule |
|---|---|
| market entry | fills at the first print after the decision, however late; with no print before the time stop it is `DATA_INCOMPLETE` (`no_print_before_time_stop`), never "not reached" |
| stop entry | fills at the trigger, or at the open when the bar gapped past it |
| limit entry or target | fills only when price trades **through** it; a touch proves nothing about the queue (`limit_touched_never_traded_through`) |
| protective stop | exits at its level, or at the open it gapped past |
| entry bar | an exit counts only where the entry mechanics prove it came after the fill; otherwise `AMBIGUOUS` |
| both barriers in one bar | `AMBIGUOUS` at any resolution, never ordered by a heuristic |
| a gap before resolution, or a source that does not cover the window | `DATA_INCOMPLETE`; retried, and recorded as incomplete 6 h after the time stop |
| time stop | exits at the close of the last bar that ended by it (the last observable price), resolved at the time stop |
| MFE / MAE | only excursions provably after the fill |

`resolution` names the rule that decided. Version 1 (`reconstruct_outcome`)
is kept unchanged for reproducing old rows; it fills a limit on a touch, and
on the raw-trade path it recorded 5.8 % of market entries (1 335 of 22 912,
2026-10-01..14) as `ENTRY_NOT_REACHED` because a quiet tape printed nothing
within a minute.

## Outcome states

`ENTRY_NOT_REACHED`, `ENTRY_EXPIRED`, `TARGET_FIRST`, `STOP_FIRST`,
`TIME_STOP`, `CANCELLED`, `AMBIGUOUS` and `DATA_INCOMPLETE` stay apart. Only
`TARGET_FIRST`, `STOP_FIRST` and `TIME_STOP` are trades with a return; from
evaluator 2 every other state stores NULL returns, and learning (posteriors,
drift, lifecycle, calibration) reads only the three trade states.

## Cost provenance

`realized_net_return` is realized gross return less the recorded round-trip cost.
Every row carries `cost_model_version` and `round_trip_cost_bps`; consumers must
preserve the version when comparing outcomes. Version 1 represents the legacy
flat-cost definition. Version 2 uses the cost carried by the proposal, so the
outcome uses the same cost definition that priced the plan.

## Candidate market outcomes

`candidate_market_outcomes` records the market path for every candidate, whether
published or not, over the configured horizons. It includes return, side-adjusted
MFE/MAE, and time to each extreme. These outcomes are based on closed one-minute
candles and retain the candidate's point-in-time identity.

Market outcomes are owned by AltStrategy. Execution outcomes such as fills,
slippage, fees, and realized account PnL belong to the execution service and are
not inferred from the market path.

## Metrics: one definition each (DESIGN-10)

| metric | definition | is not |
|---|---|---|
| fill rate | filled / (all − `DATA_INCOMPLETE`) | |
| TP-first rate | `TARGET_FIRST` / trades | a win rate: a target below the cost is hit first and loses |
| net win rate | trades with net > 0 / trades | |
| mean net per trade | bps after cost, with a cluster-bootstrap 95 % CI over (pair, UTC day) | evidence without its CI |
| net per opportunity | trade nets, plus each `AMBIGUOUS` bounded by its stop and its target, over all proposals (unfilled = 0) | |
| direction accuracy | side-correct share at horizon h against the base rate of that side and horizon | |
| probability calibration | Brier score against climatology and a reliability table, on the event the probability is defined for | transferable to another event |
| ranking value | Spearman of score against realized net; per-context lift of the top-ranked candidate, with CI | |

Only a probability that passed a calibration test may be called a confidence.
`ml_conviction` is a score (p_side − p_opposite), not a probability. Nothing but
net after cost, per opportunity, with its CI, may be called profitability.

## What the measurements say (2026-10-01..14, offline replay)

Measured on the production archive, offline, by the research tools
(`tools/outcome-replay`, research archive `vps1_signal_quality_20261015`,
outside this repository). One window: none of it is an out-of-sample proof.

- The same 20 205 BALANCED proposals: TP-first 52.8 % as recorded, 34.6 % under
  plan geometry 2 and evaluator 2; mean net −16.8 bps [−17.8, −15.7] and
  −19.2 bps [−20.5, −18.0]. The sub-bps targets inflated the hit rate; they did
  not hide an edge.
- Of 32 strategy × side cells, 23 are negative after cost with the CI below 0,
  3 inconclusive, 3 too thin, 3 without signals; none positive.
- Ranking: the champion's score has Spearman −0.031 with the fixed-horizon net
  (15m +0.045, 1h +0.023, 4h −0.079) and −0.002 with plan net; its top pick per
  context does not beat the context mean.
- AltML `p_side` against its own event (the side clears cost at the horizon):
  Brier 0.2206 against climatology 0.2273. Against the plan's target before its
  stop: 0.2369 against 0.2372, no skill. A plan probability needs its own model.
- Passive-entry fill time: 24.7 % filled within the published P50 and 38.0 %
  within the P90; 53.6 % never filled within the timeout.
