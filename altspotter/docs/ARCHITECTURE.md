# Architecture

One repository, one executable, one systemd unit, one `altspotter run` process.
Inside the process a supervisor runs cooperating components, each on its own
thread with exponential restart backoff, coordinated by one `ShutdownToken` and
one `sd_notify` readiness signal.

## Component groups

```
instrument_catalog          ─ 4-venue catalog + global universe resolver
market_gateway              ─ Bitget live candles/trades
microstructure_collector    ─ Bitget order book + raw trades
binance_usdm_runtime  ┐
bybit_runtime         ├─ per-venue live WS + context + microstructure persistence
okx_runtime           │
bitget_context_runtime┘  (Bitget derivatives context only)
market_history              ─ REST bootstrap/reconcile/repair, all venues
storage_writer              ─ venue-aware candle writer (ALTSPOTTER_BARS consumer)
bar_aggregation             ─ 5m..1d aggregates, all venues
microstructure_aggregation  ─ raw trades/books → 1s/1m aggregates
data_integrity              ─ gaps/coverage/health, all venues
retention_manager           ─ storage budget + tiered partition-drop retention
```

Disabled venues (`exchanges.<venue>.enabled: false`) are skipped by the runner;
their pairs never become eligible.

## Data flow

```
venue WS ─► VenueWsSession ─► normalize ─► Core NATS (trades/book/candle updates)
                                        └► JetStream ALTSPOTTER_BARS      (closed 1m bars)
                                        └► JetStream ALTSPOTTER_MARKET_CONTEXT (bbo/ticker/
                                             mark/index/funding/OI, throttled)
closed 1m bars ─► storage_writer ─► market_candles_1m (venue-aware UPSERT)
                                  └► bar_aggregation ─► market_candles_agg
raw trades/books ─► per-venue writer threads ─► market_trades_raw /
                    market_orderbook_snapshots (sampled, budget-gated)
                    └► microstructure_aggregation ─► 1s/1m trade & book aggregates
ticker/mark/... ─► per-minute roll ─► market_derivatives_context_1m, market_bbo_1m
REST backfill ─► market_history ─► ALTSPOTTER_BARS batches ─► storage_writer
```

## Isolation & concurrency

Per-venue runtimes are fully isolated (own REST client, rate limiter, WS
session(s), reconnect + clock state, order books, bounded queues). Shard key for
all in-memory state is `exchange_id + instrument_id`; there is no global mutex
across venues or instruments. JetStream publishes go through a bounded queue on a
dedicated thread so a NATS outage never stalls a WS io thread. PostgreSQL/NATS
outages degrade the whole process per the shared readiness policy.

## Key building blocks

- `LocalBook` — venue-agnostic order-book engine with per-venue sequencing.
- `VenueWsSession` / `VenueWsProtocol` — generic supervised WS + per-venue parser.
- `JsPublishQueue` — bounded, drop-oldest JetStream publisher thread.
- `PersistenceGate` — process-wide storage-pressure mode.
- `PgPool` — bounded PostgreSQL connection pool (retryable classification).
- Exact `Decimal` fixed-point — no float money anywhere.
