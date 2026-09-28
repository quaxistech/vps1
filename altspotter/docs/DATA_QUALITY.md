# Data quality & availability

## Availability semantics

Absent values are never encoded as zero. Consumers distinguish:

| State | Meaning |
|---|---|
| `VALID` | fresh, sequence-consistent data |
| `DEGRADED` | reachable but stale/incomplete or venue reconnecting |
| `STALE` | no fresh update within the freshness window |
| `DESYNCED` | order book failed sequence/checksum validation |
| `NOT_AVAILABLE` | the venue did not deliver the value this cycle |
| `NOT_APPLICABLE` | the field does not exist for this market type |

Proto: optional fields left unset = `NOT_AVAILABLE`; `Envelope.quality_state`
and `QualityState` carry the explicit state. In SQL, NULL context columns mean
the venue did not report them.

## Per-instrument quality (`exchange_data_quality`)

The venue runtimes maintain, per `(exchange_id, instrument_id)`:
trade/candle/book/ticker/mark/OI freshness ages, sequence-gap count, reconnect
count, REST/WS error counts, crossed-book count, rejected-payload count and an
overall `quality`. Refreshed every ~30 s.

`altspotter inspect --quality` prints the current per-venue matrix.

## Connection state (`exchange_connections`)

Per venue: `STARTING | HEALTHY | DEGRADED | RECONNECTING | STALE |
DISABLED_BY_CONFIG | UNAVAILABLE`, with reconnect count and timestamps.
`altspotter inspect --exchanges` shows the live summary.

## Clock offset (`exchange_clock_offsets`)

Each runtime tracks an EWMA of `exchange_event_time − local_receive_time` per
venue, so downstream can detect venue clock skew.

## Candle integrity

`data_integrity` runs per `(exchange_id, instrument)`: minute-grid gap detection
(`history_gaps`), coverage refresh (`history_coverage`), bounded repair dispatch
with exponential backoff, and a health state machine published to
`evt.integrity.<venue>.<symbol>.health`.

A detected gap becomes a `RepairCommand` on `cmd.history.>`; `market_history`
re-fetches the range from that venue's REST history endpoint and republishes it,
and the gap closes automatically once every minute in its range is present. A
gap that survives max_gap_repair_attempts is marked unrepairable and retained
with its coverage record. This distinguishes an unresolved historical gap from
a range that has been fully repaired.

## Comparability across venues

Every venue is sampled on the **same schedule**: order-book snapshots follow the
shared `sampling.orderbook_{core,other}_interval_ms` cadence keyed on the
instrument's retention class, on all four venues including Bitget. This is what
makes the 1s/1m book aggregates comparable — a venue sampled more densely would
produce depth/imbalance/spread statistics computed from a different number of
observations, which is not a market difference but an artefact of collection.

A price on one venue deviating sharply from the four-venue median is a **quality
warning**, never a trading signal — altspotter produces market data and quality
metadata only; cross-venue features and signals belong to downstream consumers.
