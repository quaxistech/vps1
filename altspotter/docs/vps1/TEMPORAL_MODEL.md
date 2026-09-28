# Temporal model

Market event time, local receive time, persistence time, feature time, decision time, inference time, and outcome time describe different events. They must remain distinct throughout collection and downstream processing.

## Time fields

| Time | Meaning |
|---|---|
| Event time | When the venue reports that a market observation occurred |
| Receive time | When AltSpotter received the observation |
| Persist time | When the record was committed to durable storage |
| Feature time | The bar or window described by a derived feature |
| Decision time (T0) | When a downstream decision context was captured |
| Inference time | When a model generated a prediction |
| Outcome time | When the evaluated market horizon completed |

AltSpotter stores timestamps in UTC and preserves venue event time separately from local receive and persistence time. See [Market-data semantics](../MARKET_DATA_SEMANTICS.md) for field names.

## Point-in-time rules

1. A feature is identified by its source interval and source revision.
2. A corrected source record creates a corresponding revised derived record.
3. A decision context is immutable after capture; a later context is a new record.
4. A decision uses only observations available at or before its T0.
5. Training labels use the future outcome window only after that window has closed.
6. Freshness windows are explicit configuration, not inferred from storage time.

The allowed and forbidden inputs at T0 are listed in [Point-in-time data rules](DATA_LEAKAGE_MODEL.md).
