# Connecting to altspotter — data access for other projects

`altspotter` collects USDT-margined **perpetual futures** market data from four
venues — **Bitget** (`bitget`), **Binance USDⓈ-M** (`binance_usdm`), **Bybit
linear** (`bybit`) and **OKX USDT swap** (`okx`) — and publishes everything two
ways:

- **NATS** — low-latency **live** stream (trades, 1m candle updates, closed bars,
  order-book snapshots/deltas, BBO, ticker, mark/index/funding/open-interest,
  instrument/gateway/integrity/universe events). Use this to react in real time.
- **PostgreSQL** — the durable **history** and current state (all candles, order
  books, raw trades, derivatives context, aggregates, catalogue, coverage,
  universe). Use this for backfill, backtests, features and anything that must
  survive a restart.

**Every market key is `(exchange_id, market_type, instrument_id)`.** `exchange_id`
is `1` bitget, `2` binance_usdm, `3` okx, `4` bybit (also the proto `Exchange`
enum). `instrument_id` is the **canonical pair id** shared across venues (the same
BTC-USDT perpetual has one `instrument_id` and four `exchange_instruments` rows).
`market_type` is `usdt_perp` for all supported venues.

**Global observation universe.** A pair is present in the live/stored data only
when it is active on **all four** venues (`global_observation_universe.eligible`).
A pair missing on one venue is excluded everywhere; read the current set from
`global_observation_universe` or `evt.universe.*`.

> JetStream is a transport, **not** the system of record. For history, read
> PostgreSQL. For live reaction, subscribe to NATS.

Both PostgreSQL and NATS bind to **`127.0.0.1` only**. A consumer must run on the
**same host** as altspotter, or reach it through an SSH tunnel
(`ssh -L 5432:127.0.0.1:5432 -L 4222:127.0.0.1:4222 …`). Nothing is exposed to
the internet.

---

## 1. PostgreSQL

### Connection

Database `altspotter` on `127.0.0.1:5432`. altspotter services connect with the DSN in
`/etc/altspotter/common.env` (`ALTSPOTTER_POSTGRES_DSN`, URI form
`postgresql://<user>:<pw>@127.0.0.1:5432/altspotter`). **Do not** reuse the writer's
credentials for a read-only consumer — create a least-privilege role:

```sql
CREATE ROLE altspotter_reader LOGIN PASSWORD '<generate-a-strong-password>';
GRANT CONNECT ON DATABASE altspotter TO altspotter_reader;
GRANT USAGE ON SCHEMA public TO altspotter_reader;
GRANT SELECT ON ALL TABLES IN SCHEMA public TO altspotter_reader;
ALTER DEFAULT PRIVILEGES IN SCHEMA public GRANT SELECT ON TABLES TO altspotter_reader;
```

All timestamps are **UTC** (`TIMESTAMPTZ`). Prices/sizes/volumes are stored as
`NUMERIC` — read them as decimal strings; never cast through binary `float`.

### Resolve a symbol to `instrument_id`

Market tables key on `(exchange_id, instrument_id)`, not the ticker. Join the
catalogue to translate:

```sql
-- one venue's contract for a base asset
SELECT ei.exchange_id, ei.instrument_id
FROM exchange_instruments ei JOIN exchanges e ON e.id = ei.exchange_id
WHERE e.code = 'okx' AND ei.exchange_symbol = 'BTC-USDT-SWAP'
  AND ei.status = 'active' AND ei.is_whitelisted;

-- the canonical pair id shared across all four venues
SELECT id FROM canonical_pairs WHERE base_asset = 'BTC' AND quote_asset = 'USDT';
```

`exchange_id` is data: `1` bitget, `2` binance_usdm, `3` okx, `4` bybit. The venue
symbol differs (`BTCUSDT` vs `BTC-USDT-SWAP`) — never treat equal strings as the
same instrument; join through `instrument_id`/`canonical_pairs`.

### Tables you read

| Table | What | Key |
|---|---|---|
| `market_candles_1m` | canonical 1-minute OHLCV (+ taker split, quality, revision) | `(exchange_id, instrument_id, open_time)` |
| `market_candles_agg` | 5m/15m/30m/1h/4h/1d aggregates | `(exchange_id, instrument_id, timeframe, open_time)` |
| `market_derivatives_context_1m` | per-minute mark/index/funding/OI/BBO/turnover | `(exchange_id, instrument_id, open_time)` |
| `market_bbo_1m` | per-minute best bid/ask summary | `(exchange_id, instrument_id, open_time)` |
| `market_trade_aggregates_1s` / `_1m` | order-flow aggregates (buy/sell vol, VWAP, signed vol) | `(exchange_id, instrument_id, open_time)` |
| `market_book_aggregates_1s` / `_1m` | book depth/imbalance/microprice summary | `(exchange_id, instrument_id, open_time)` |
| `market_orderbook_snapshots` | sampled top-15 book snapshots, same cadence on every venue (ephemeral) | `(retention_class, exchange_id, instrument_id, snapshot_time)` |
| `market_trades_raw` | raw public trades, aggressor side (ephemeral) | `(retention_class, exchange_id, instrument_id, trade_time, trade_id)` |
| `market_funding_settlements` | funding settlement history | `(exchange_id, instrument_id, funding_time)` |
| `canonical_pairs`, `exchange_instruments`, `canonical_pair_exchange_map` | catalogue, specs, per-venue mapping | — |
| `global_observation_universe`, `global_universe_events` | four-venue eligibility + transitions | — |
| `history_coverage`, `history_gaps`, `ingestion_runs` | coverage, gaps, backfill audit | — |
| `exchange_connections`, `exchange_data_quality` | per-venue connection + quality state | — |

**Retention & durability classes.** `market_candles_*`, `market_derivatives_context_1m`,
aggregates and catalogue are **CORE_DURABLE** (long retention, in daily backup).
`market_trades_raw` and `market_orderbook_snapshots` are **EPHEMERAL** (short
retention, daily-partitioned by `retention_class` core/other; rebuilt-forward from
live, not in backup) — build from the aggregates for anything older than a few
days. Never assume raw trades/books exist beyond their retention window.

`market_candles_1m` is **monthly RANGE-partitioned** on `open_time` — always
filter by a time range so the planner prunes partitions.

`market_orderbook_snapshots` columns: `snapshot_time, seq, best_bid, best_ask,
mid, microprice, spread_bps, bid_depth_n, ask_depth_n, imbalance_n, levels,
bids JSONB, asks JSONB` (`bids`/`asks` are `[[price,size],…]`, up to 15 levels).

`market_trades_raw` columns: `trade_time, trade_id, price, size,
side ('buy'|'sell' = aggressor)`.

### Example reads

```sql
-- last 24h of 1m candles for WLD
SELECT open_time, open, high, low, close, base_volume
FROM market_candles_1m
WHERE exchange_id = 1 AND instrument_id = :iid
  AND open_time >= now() - interval '24 hours' AND is_closed
ORDER BY open_time;

-- current book imbalance stream (last 5 min)
SELECT snapshot_time, mid, microprice, imbalance_n, spread_bps, levels
FROM market_orderbook_snapshots
WHERE exchange_id = 1 AND instrument_id = :iid
  AND snapshot_time >= now() - interval '5 minutes'
ORDER BY snapshot_time;

-- is coverage healthy before you trust a backtest?
SELECT status, earliest_1m, covered_until, gap_count
FROM history_coverage WHERE exchange_id = 1 AND instrument_id = :iid;
```

For change-driven consumers, poll `market_orderbook_snapshots` /
`market_trades_raw` by `snapshot_time`/`trade_time > :last_seen`, or subscribe to
NATS (below) instead of polling.

---

## 2. NATS / JetStream

### Connection

`nats://127.0.0.1:4222`, authentication enabled. Credentials live in the file
referenced by `nats_credentials_file` in `/etc/altspotter/common.yaml`
(`/etc/altspotter/*.creds`, mode `0640`, owner `root:altspotter`). Monitoring (read-only) is on
`127.0.0.1:8222`. Use the official NATS client for your language.

### Subjects (Core NATS — live, no retention)

| Subject | Payload | Notes |
|---|---|---|
| `md.<venue>.<symbol>.trade` | `Trade` | every public trade, normalized (size in base units) |
| `md.<venue>.<symbol>.candle.1m.update` | `Candle` | in-progress 1m candle; **not final** |
| `md.<venue>.<symbol>.book.snapshot` | `OrderBookSnapshot` | top-15 after (re)sync |
| `md.<venue>.<symbol>.book.delta` | `OrderBookDelta` | incremental book updates (size 0 deletes) |

`<venue>` ∈ `bitget|binance_usdm|bybit|okx`; `<symbol>` is that venue's native
symbol. These are fire-and-forget — a late subscriber gets only new messages.
Raw trades and full book deltas are Core NATS only (never long-retained).

### Subjects (JetStream — retained, replayable)

Stream **`ALTSPOTTER_BARS`** — subjects `bar.>`, file storage, 30-day age, ~2 GiB,
discard-old, 10-minute dedup window:

| Subject | Payload | Notes |
|---|---|---|
| `bar.bitget.<symbol>.1m.closed` | `Candle` | **finalized** 1m candle (dedup by msg-id) |
| `bar.batch.agg` | `CandleBatch` | closed higher-timeframe aggregates |

Stream **`ALTSPOTTER_CONTROL`** — subjects `cmd.history.>`, `evt.instrument.>`,
`evt.history.>`, `evt.integrity.>`, `evt.gateway.>`, `evt.storage.>`, file
storage, 30-day, explicit-ack:

| Subject | Payload |
|---|---|
| `evt.instrument.<venue>.{snapshot,created,updated,enabled,disabled}` | `InstrumentSnapshot` / `InstrumentEvent` |
| `evt.gateway.<venue>.{connected,disconnected,degraded,recovered}` | `GatewayEvent` |
| `evt.integrity.<venue>.<symbol>.{health,book_desynced,book_recovered}` | `IntegrityEvent` |
| `evt.history.<venue>.<symbol>.repair.{completed,failed}` | `RepairResult` |
| `evt.storage.{state,cleanup,critical}` | `IntegrityEvent` (budget state) |
| `cmd.history.<venue>.<symbol>.repair` | `RepairCommand` |

Stream **`ALTSPOTTER_MARKET_CONTEXT`** — subjects `md.*.*.{bbo,ticker,mark,index,funding,
open_interest}`, `evt.universe.>`, file storage, 14-day, discard-old:

| Subject | Payload |
|---|---|
| `md.<venue>.<symbol>.bbo` | `BestBidAsk` |
| `md.<venue>.<symbol>.ticker` | `MarketTicker` |
| `md.<venue>.<symbol>.{mark,index}` | `MarkPrice` / `IndexPrice` |
| `md.<venue>.<symbol>.funding` | `FundingRate` |
| `md.<venue>.<symbol>.open_interest` | `OpenInterest` |
| `evt.universe.{snapshot,pair.eligible,pair.excluded,pair.changed}` | `GlobalUniverseSnapshot` / `GlobalUniverseChange` |

### Consuming JetStream — create your OWN durable

altspotter owns these durables — **do not attach to them**, you would steal their
messages: `ALTSPOTTER_BARS/storage-writer-bars`, `ALTSPOTTER_CONTROL/history-repair`,
`ALTSPOTTER_CONTROL/integrity-repair-events`, `ALTSPOTTER_CONTROL/aggregation-repair-events`.

Create a **new, uniquely-named** durable pull consumer with your own filter, e.g.
a strategy that wants closed WLD bars:

```
stream:   ALTSPOTTER_BARS
durable:  myproj-wld-bars        # unique to your project
filter:   bar.bitget.WLDUSDT.1m.closed
ack:      explicit  (ack AFTER you have processed/persisted)
```

Ack only after successful processing so a crash re-delivers, never before.

### Decoding messages

Every payload is a versioned **`Envelope`** (Protobuf) wrapping the typed message.
Envelope fields: `schema_version, message_id, event_type, exchange,
instrument_id, exchange_symbol, exchange_timestamp, produced_at, trace_id,
source_service, payload`. Inner types live in `proto/` (`candle.proto`,
`trade.proto`, `instrument.proto`, `history.proto`, `common.proto`).

To decode in your project, **compile the same `proto/` files** (they are the
contract — keep them byte-identical to altspotter's). Then:

1. parse bytes → `Envelope`;
2. check `schema_version` (unknown fields must not break you — reject on major
   version change only);
3. parse `Envelope.payload` → the message named by `event_type`
   (`md.trade`→`Trade`, `md.candle.update`/`bar.closed`→`Candle`, …).

Decimal money fields use the `Decimal` message (mantissa + scale) or NUMERIC-safe
strings — never reconstruct them through `double`.

---

## 3. Which channel for which job

| You want… | Use |
|---|---|
| react to each trade / live 1m tick | Core NATS `md.bitget.*` |
| act on a **confirmed** closed candle | JetStream `bar.bitget.*.1m.closed` |
| replay recent bars after downtime | JetStream `ALTSPOTTER_BARS` (your durable) |
| backfill / backtest / features / order-flow history | PostgreSQL tables |
| know if an instrument went active/disabled | `evt.instrument.*` + `exchange_instruments` |
| check data quality before trusting it | `history_coverage`, `history_gaps` |

---

## 4. Checklist for your project

- Compile altspotter's **`proto/`** (identical bytes) — the wire contract.
- Point at the shared **PostgreSQL schema** (`db/migrations/` from altspotter);
  do not run migrations that alter market-data tables — altspotter owns them.
- Use a **read-only DB role** and your **own JetStream durable name**.
- Everything is **UTC** and **NUMERIC/Decimal** — no float money.
- Connect over **localhost/SSH tunnel** only.
