# Data model

The instrument catalogue and provider bindings define what each stored series
means. `macro_instruments` identifies the series; `macro_instrument_sources`
maps it to a provider symbol and records its normalization semantics.

## Series classes

| class | meaning | storage |
|---|---|---|
| `MARKET` | A traded or quoted market series | `macro_price_bars` |
| `OFFICIAL_REFERENCE` | A published reference rate, fixing or index | `macro_reference_values` |
| `ECONOMIC_RELEASE` | A statistic tied to an observation period and possibly revised | `macro_events` |
| `DERIVED` | A documented calculation from other series | Defined by the instrument's `derivation` |

## Time and source semantics

All stored timestamps are normalized to UTC. A source binding records its
native timezone and timestamp meaning (`bar_open`, `bar_close`,
`point_in_time` or `observation_date`). Market bars retain event time and
ingest time. Reference rows can also retain publication time and the source
time represented by a carried-forward value. Release rows retain observation
time, publication time, vintage date and revision.

For point-in-time use, select data by when it became available, not only by the
period it describes. If a source does not publish an availability timestamp,
`ingest_time` is the recorded bound on when this service learned the value.

Price basis (`bid`, `ask`, `mid`, `last`, `close` or `fixing`) is part of the
series identity. Provider fallback is resolved within the same basis.

## Quality and absence

| status | meaning | value allowed |
|---|---|---|
| `VALID` | Complete observation accepted by validation | Required |
| `STALE` | Last known value carried forward, with its source time recorded | Allowed |
| `MISSING` | An observation was expected but absent | No |
| `PROVIDER_ERROR` | The source failed or returned unusable data | No |
| `MARKET_CLOSED` | No observation was expected for the session | No |
| `PARTIAL` | The bar or period is incomplete | Bars may retain partial OHLC |

The database constraints enforce the value rules. Consumers can distinguish a
closed session from missing data and a provider failure without interpreting a
gap or inventing a zero.

## Coverage and revisions

Bootstrap cursors and gap records track requested coverage. The first available
source timestamp is recorded from retrieved data; older history is not filled
with generated values. Ingest is repeatable and upserts revisions to price and
reference rows. Economic release vintages coexist so a revised statistic does
not erase the value previously published.

Useful SQL definitions are in `db/queries/series_semantics.sql` and
`db/queries/downstream_aligned_context.sql`.
