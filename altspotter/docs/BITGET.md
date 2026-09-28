# Bitget USDT-M futures

Bitget ingestion is split across three components:

- `market_gateway` — live 1m candles and public trades;
- `microstructure_collector` — order book (`books15`) and raw trades;
- `bitget_context_runtime` — derivatives context.

The WS/REST plumbing lives in
[`src/exchanges/bitget/bitget_ws.cpp`](../src/exchanges/bitget/bitget_ws.cpp),
[`bitget_adapter.cpp`](../src/exchanges/bitget/bitget_adapter.cpp) and
[`bitget_context.cpp`](../src/exchanges/bitget/bitget_context.cpp).

The v2 public `ticker` channel supplies mark price, index price, funding rate,
next funding time, open interest (`holdingAmount`), 24 h turnover and best
bid/ask. These are published on `ALTSPOTTER_MARKET_CONTEXT` and rolled into
`market_derivatives_context_1m`, the same context collected on the other three
venues.

## Endpoints

- REST `https://api.bitget.com`, `productType=USDT-FUTURES`
  (`/api/v2/mix/market/*`).
- WS `wss://ws.bitget.com/v2/ws/public` (client `ping` text ≤ 30 s).

`exchange_id = 1`, subject label `bitget`, symbol form `BASEUSDT` (e.g.
`BTCUSDT`). Contract sizes are already in base units.
