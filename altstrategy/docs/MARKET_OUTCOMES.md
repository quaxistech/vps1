# Market outcomes

AltStrategy computes proposal outcomes from AltSpotter's durable market history.
This records what the market did after a proposal; it is separate from any
execution result.

## Path reconstruction

The outcome reader uses the finest eligible path available:

1. raw trades;
2. one-second trade aggregates;
3. one-second price/book aggregates;
4. one-minute candles as the fallback.

The selected path and its source are recorded with the outcome.

## Outcome states

`ENTRY_NOT_REACHED`, `ENTRY_EXPIRED`, `TARGET_FIRST`, `STOP_FIRST`,
`TIME_STOP`, `CANCELLED`, `AMBIGUOUS`, and `DATA_INCOMPLETE` distinguish entry,
exit, and data-quality cases. Outcomes also carry entry and exit prices, gross
and estimated net return, MFE, MAE, and holding duration.

If stop and target both occur inside a bar whose internal order is unknown, the
outcome is `AMBIGUOUS`; it is not assigned an assumed win or loss. A finer path
may resolve the ordering. Ambiguous and incomplete outcomes remain archived and
are excluded from binary-label training.

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
