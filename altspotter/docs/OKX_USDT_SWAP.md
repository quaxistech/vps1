# OKX USDT-margined swap

Adapter: [`src/exchanges/okx/okx_adapter.hpp`](../src/exchanges/okx/okx_adapter.hpp).
The endpoints and channels below describe the integration implemented by AltSpotter.

## REST — `https://www.okx.com`

| Purpose | Endpoint | Notes |
|---|---|---|
| instruments | `/api/v5/public/instruments?instType=SWAP` | keep `ctType=linear`, `settleCcy=USDT`; `ctVal` = base units per contract |
| 1m candles (recent) | `/api/v5/market/candles?bar=1m&limit=300` | |
| 1m candles (history) | `/api/v5/market/history-candles?bar=1m&limit=100&after=<ms>` | `after` = strictly-earlier cursor (matches the exclusive-end walk) |
| funding history | `/api/v5/public/funding-rate-history?limit=100` | |
| open interest | `/api/v5/public/open-interest?instType=SWAP` | `oiCcy` (base) + `oiUsd` |

Rate limit for history-candles ≈ **20 req / 2 s / IP**; configured
`rate_limit_per_second: 4`. `state`: `live` → active; `suspend/preopen/test` →
suspended; `expired` → delisted. Candle row:
`[ts,o,h,l,c,vol(contracts),volCcy(base),volCcyQuote(quote),confirm]` — base and
quote volume come from `volCcy`/`volCcyQuote`.

## Candle confirmation

The adapter uses the `confirm` flag for the newest candle and treats older
historical windows as final after the configured close-time grace period.

## Contract-value normalization

OKX trade and order sizes are in **contracts**; the runtime multiplies by the
instrument's `ctVal` (exact decimal) to store base units, so trade/candle volumes
are comparable across venues. `lotSz`/`minSz` are likewise normalized in the
catalog.

## WebSocket

Two endpoints (public data + business candles), heartbeat = text `"ping"` when
idle > ~20 s (expect `"pong"`).

- Public `wss://ws.okx.com:8443/ws/v5/public`: `trades`, `books`, `tickers`,
  `mark-price`, `funding-rate`, `open-interest`.
- Business `wss://ws.okx.com:8443/ws/v5/business`: `candle1m`.

Order book uses the standard public **`books`** channel (400 depth, no VIP):
`action=snapshot`/`update` with `seqId`/`prevSeqId` chaining (see
[ORDERBOOKS](ORDERBOOKS.md)). The `checksum` field is deprecated and always 0,
so crc32 verification only runs when a non-zero value is present. Premium/VIP-only
channels are never required.
