# Binance USDⓈ-M futures

Adapter: [`src/exchanges/binance/binance_adapter.hpp`](../src/exchanges/binance/binance_adapter.hpp).
The endpoints and channels below describe the integration implemented by AltSpotter.

## REST — `https://fapi.binance.com`

| Purpose | Endpoint | Notes |
|---|---|---|
| instruments | `/fapi/v1/exchangeInfo` | keep `contractType=PERPETUAL`, `quoteAsset=marginAsset=USDT` |
| 1m klines | `/fapi/v1/klines?interval=1m&limit=1500` | ascending; array form; taker-buy in field [9] |
| funding history | `/fapi/v1/fundingRate?limit=1000` | |
| open interest | `/fapi/v1/openInterest` | base units |
| mark/index/funding | `/fapi/v1/premiumIndex` | |
| depth snapshot | `/fapi/v1/depth?limit=1000` | book resync seed |

Rate limit: **2400 request-weight / minute / IP** (klines `limit=1500` costs 10).
Configured `rate_limit_per_second: 3`. `status`: `TRADING` → active; `SETTLING/
PENDING_TRADING/…` → suspended; `CLOSE/DELIVERING` → delisted.

## WebSocket — two endpoints by stream group

Binance uses separate base URLs for market and public streams. AltSpotter opens
one connection for each group:

| Base URL | Streams subscribed here |
|---|---|
| `wss://fstream.binance.com/market/stream` | `@aggTrade`, `@kline_1m`, `@markPrice` (3 s), `@ticker` |
| `wss://fstream.binance.com/public/stream` | `@depth` (diff depth, 250 ms) |

Both are configurable (`ws_url`, `ws_public_url`). One `SUBSCRIBE` per batch
(respect the **10 msgs/s** inbound cap). Server sends protocol pings (Beast
auto-answers); connections are force-closed at **24 h** so sessions rotate
proactively at 23 h. ≤ 1024 streams / connection.

- `aggTrade.m=true` ⇒ buyer is maker ⇒ taker sold (aggressor = sell).
- `depthUpdate` fields `U/u/pu` drive the local book (see [ORDERBOOKS](ORDERBOOKS.md)).
- `markPriceUpdate` carries mark `p`, index `i`, funding `r`, next funding `T`.

Sizes are already in base units; no contract-value conversion needed.
