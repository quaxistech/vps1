# Strategy economics and ranking

## Cost estimate

AltStrategy records a versioned round-trip cost with each proposal. The current
cost model combines entry and exit fees, spread cost when a leg crosses the
book, and a depth-impact estimate. It uses fee inputs from `altml.fee_models`
and market context available at the proposal's T0.

The depth-impact term is an estimate based on available liquidity context; it is
not a size-aware order-book walk. The proposal records the cost model version
and cost components so downstream consumers can interpret the estimate.

## Ranking

The ranking policy produces a deterministic ordering from versioned candidate
components. Its output describes relative candidate priority within AltStrategy;
it does not authorize execution or size a position. Policy parameters are
stored with their version and remain separate from market-outcome measurements.

## Estimated edge

An expected net return is present only when AltML supplied a usable market
forecast. If no forecast is available, `edge_estimated` is false,
`expected_net_return` and `p_target_before_stop` remain absent, and the proposal
carries `NO_EDGE_ESTIMATE`. AltStrategy does not invent a probability estimate
or use an economic estimate as a publication gate.

Reward/risk geometry remains available from the proposal's stop and target
levels; it is not a forecast or an execution result. Market outcome and
execution outcome are separate records. See [Market outcomes](MARKET_OUTCOMES.md)
and [Prediction contract](PREDICTION_CONTRACT.md).
