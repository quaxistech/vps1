# Global observation universe

The set of pairs altspotter actually observes is the **four-venue intersection**
of the configured whitelist:

```
global_observation_universe =
    configured_bases
    ∩ active_bitget_usdt_perpetuals
    ∩ active_binance_usdt_perpetuals
    ∩ active_bybit_usdt_perpetuals
    ∩ active_okx_usdt_perpetual_swaps
```

The configured bases come from `instrument_catalog.symbols_file` (base asset
only — one source of truth, no second hardcoded list). A base is **eligible**
only when a tradable linear USDT perpetual is confirmed **active on every
enabled venue**.

Only list a base that trades as a plain `<BASE>USDT` linear perpetual on all
enabled venues. Venue-exclusive listings and venue-specific multiplier contracts
(e.g. `1000PEPEUSDT` on one venue but `PEPEUSDT` on another) are not the same
instrument, would never become eligible, and only add noise to the exclusion
log.

## Hard rule

If a configured pair is missing or inactive on even one venue it is **globally
excluded**. For an excluded pair, on *every* venue:

- no WebSocket subscription;
- no historical backfill;
- no stored candles / trades / books / derivatives context;
- no published market-data subjects;
- not counted as active universe.

This is enforced through `exchange_instruments.is_whitelisted`: the resolver sets
it to `eligible` for **all** of a pair's venue rows at once, and every downstream
component subscribes/backfills/stores only `is_whitelisted AND status='active'`.
So the intersection is inherited automatically — Bitget, Binance, Bybit and OKX
components, the history backfiller, aggregation and integrity all respect it
without special-casing.

## Statuses

`global_observation_universe.status` ∈
`ELIGIBLE_ALL_VENUES`, `MISSING_ON_BITGET`, `MISSING_ON_BINANCE`,
`MISSING_ON_BYBIT`, `MISSING_ON_OKX`, `INACTIVE_ON_VENUE`, `SPEC_MISMATCH`,
`TEMPORARILY_UNVERIFIED`, `EXCLUDED`. The per-venue `*_listed` / `*_active` flags
and a human `reason` are stored alongside.

## Transient outage ≠ delisting

If any enabled venue's contract list fails to fetch this cycle, eligibility is
**not** recomputed: the previous whitelist is preserved and the universe is left
as-is (only the affected venue's rows go untouched). One network blip therefore
never mass-drops pairs. New pairs are added only once presence on all four venues
is positively confirmed.

## Re-evaluation & events

The catalog re-syncs every `instrument_catalog.sync_interval_sec` (default 15 min;
`universe.reevaluate_interval_sec` documents the intent). Transitions write
`global_universe_events` and publish `evt.universe.{snapshot,pair.eligible,
pair.excluded,pair.changed}` on the `ALTSPOTTER_MARKET_CONTEXT` stream (payloads
`GlobalUniverseSnapshot` / `GlobalUniverseChange`).

## Inspecting

```
altspotter inspect --universe
```

lists each base, its status, eligibility, per-venue active flags and the
exclusion reason.

## Canonical identity

The same pair across venues shares one `canonical_pairs.id` (== `instrument_id`);
`canonical_pair_exchange_map` records which `exchange_instruments` row realizes it
on each venue. Equal symbol strings are never treated as identity — Binance
`BTCUSDT`, Bybit `BTCUSDT` and OKX `BTC-USDT-SWAP` map to the same canonical pair
by base/quote/market type, not by string.
