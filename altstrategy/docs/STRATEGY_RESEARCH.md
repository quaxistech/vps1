# Strategy research workflow

`altstrategy-replay` evaluates the strategy catalogue against archived decision
contexts and the corresponding market path. It supports replay, walk-forward
stability, ablation, venue, and timeframe analyses, and can isolate one strategy.

```sh
altstrategy-replay --from YYYY-MM-DD --to YYYY-MM-DD
altstrategy-replay --from YYYY-MM-DD --to YYYY-MM-DD --mode walkforward --folds N
altstrategy-replay --from YYYY-MM-DD --to YYYY-MM-DD --mode ablation
altstrategy-replay --from YYYY-MM-DD --to YYYY-MM-DD --mode venues
altstrategy-replay --from YYYY-MM-DD --to YYYY-MM-DD --mode grids
altstrategy-replay --from YYYY-MM-DD --to YYYY-MM-DD --only STRATEGY_ID --cost-bps BPS
```

## Evaluation rules

- Replay archived point-in-time `decision_context_snapshots` in event-time order.
- Use the same outcome reconstruction and price-source precedence as the live
  outcome tracker.
- Report unfilled entries separately from trades and preserve ambiguous or
  incomplete outcomes instead of assigning an assumed result.
- Report gross and net returns alongside each other using the configured cost.
- Walk-forward folds are contiguous in event time. They measure stability of
  the selected parameters; they do not turn replay results into an independent
  holdout.
- Ablation removes an input group. A strategy requiring an absent group is
  ineligible for that context; the group is not replaced with zero.

Replay output is generated from the selected time range and current archive.
The repository does not keep a permanent table of results in this guide.

## Recomputing archived outcomes

`altstrategy-outcome-replay --dir EXPORT_DIR --out FILE.csv` recomputes every
archived proposal from an exported dataset, without a database, and keeps each
effect apart:

| column group | geometry | evaluator | path |
|---|---|---|---|
| `v1c_*` | as recorded | 1 | 1m candles |
| `v2c_*` | as recorded | 2 | 1m candles |
| `g2_*` | rebuilt by the current `generate_plans()` from the inputs archived at T0 | 2 | 1m candles |

A proposal whose candidate, context or prediction has left the archive gets no
`g2`, and says so. Raw trades are kept for hours, so a historical replay runs on
1m candles: more `AMBIGUOUS` than live, reported as such. Changes to geometry or
evaluator are measured this way, side by side, never by updating stored
outcomes. `--g2-time-stop-horizons K` ends the rebuilt plan at K times the
hypothesis' own horizon (the DESIGN-01 study).
