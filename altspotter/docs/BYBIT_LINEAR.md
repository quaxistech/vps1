# Bybit Linear USDT perpetual

Adapter: [`src/exchanges/bybit/bybit_adapter.hpp`](../src/exchanges/bybit/bybit_adapter.hpp).
The endpoints and channels below describe the integration implemented by AltSpotter.

## REST — `https://api.bybit.com`

| Purpose | Endpoint | Notes |
|---|---|---|
| instruments | `/v5/market/instruments-info?category=linear` | cursor-paginated; keep `contractType=LinearPerpetual`, `settleCoin=USDT` |
| 1m klines | `/v5/market/kline?category=linear&interval=1&limit=1000` | **newest-first** → sorted ascending by the adapter |
| funding history | `/v5/market/funding/history?category=linear` | |
| ticker / OI | `/v5/market/tickers?category=linear` | `openInterest` (base) + `openInterestValue` (USDT) |

Rate limit: **600 requests / 5 s / IP** (shared); breach → `retCode 10006`,
10-min ban. Configured `rate_limit_per_second: 8`. `status`: `Trading` → active;
`PreLaunch/Settling/Delivering` → suspended; `Closed` → delisted. `fundingInterval`
is in minutes.

## WebSocket — `wss://stream.bybit.com/v5/public/linear`

Application heartbeat `{"op":"ping"}` every **20 s** (pong `ret_msg:"pong"`);
idle disconnect ~10 min. Per symbol: `publicTrade.<s>`, `kline.1.<s>`,
`orderbook.50.<s>`, `tickers.<s>`. ≤ 500 new connections / 5 min / domain.

- `publicTrade`: `S` taker side, `v` size, `p` price, `i` trade id, `T` ms.
- `kline.1`: `confirm=true` marks the closed minute.
- `orderbook.50`: `type=snapshot` (and `u==1`) resets; deltas need increasing
  `u`; size 0 deletes (see [ORDERBOOKS](ORDERBOOKS.md)).
- `tickers`: **snapshot/delta** — absent fields are unchanged; carries
  `markPrice`, `indexPrice`, `fundingRate`, `nextFundingTime`, `openInterest(+Value)`,
  `turnover24h`, `bid1/ask1`.
