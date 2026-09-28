# Exchanges

altspotter collects USDT-margined **linear perpetual futures** from four venues
in one process. Spot, coin-margined and delivery futures are out of scope.

| Venue | `exchange_id` | DB code / subject label | Market |
|---|---|---|---|
| Bitget | 1 | `bitget` | USDT-M perpetual |
| Binance | 2 | `binance_usdm` | USDⓈ-M perpetual |
| OKX | 3 | `okx` | USDT-margined SWAP |
| Bybit | 4 | `bybit` | Linear USDT perpetual |

`exchange_id` matches both the PostgreSQL `exchanges.id` seed and the proto
`Exchange` enum. The venue registry lives in
[`src/exchanges/venues.hpp`](../src/exchanges/venues.hpp).

## Adapter layer

Each venue implements `ExchangeMarketDataAdapter`
([`src/exchanges/market_data_adapter.hpp`](../src/exchanges/market_data_adapter.hpp)):
instrument listing, candle pagination, funding history, open interest, order-book
snapshot and error mapping. Live streaming uses a shared, venue-agnostic
WebSocket engine (`VenueWsSession`) driven by a per-venue `VenueWsProtocol`
strategy (URLs, subscription frames, heartbeat, frame parsing).

| Adapter | REST | WebSocket |
|---|---|---|
| `BitgetMarketDataAdapter` (+ `BitgetTickerWsProtocol`) | `/api/v2/mix/market/*` | `/v2/ws/public` (candles/trades/book via the dedicated components; ticker for context) |
| `BinanceUsdmMarketDataAdapter` (+ `BinanceWsProtocol`) | `/fapi/v1/*` | `/market/stream` + `/public/stream` |
| `BybitLinearMarketDataAdapter` (+ `BybitWsProtocol`) | `/v5/market/*` | `/v5/public/linear` |
| `OkxUsdtSwapMarketDataAdapter` (+ `Okx{Public,Business}WsProtocol`) | `/api/v5/*` | `/ws/v5/{public,business}` |

Exchange-specific JSON never becomes the consumer contract — everything is
normalized into the versioned proto `Envelope` and the exchange-aware schema.

## Runtime isolation

`altspotter run` starts one supervised runtime per venue. Each owns its own
connection supervisor, REST client + rate limiter, WS session(s), reconnect
state, clock-offset estimate, per-instrument order books and bounded queues. A
fault on one venue (disconnect, desync, rate-limit, REST error) never stops the
others and never blocks the storage writer. Shared critical dependencies
(PostgreSQL, NATS) degrade the whole process per the shared readiness policy.

Sharding key for all in-memory state: `exchange_id + instrument_id` — there is no
global mutex across venues or instruments.

Per-venue docs: [BITGET](BITGET.md), [BINANCE_USDM](BINANCE_USDM.md),
[BYBIT_LINEAR](BYBIT_LINEAR.md), [OKX_USDT_SWAP](OKX_USDT_SWAP.md).
