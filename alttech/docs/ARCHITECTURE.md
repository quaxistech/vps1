# Architecture

## One process, six modules

`alttech-service` is a single process. Inside it, six modules run on their own
threads under a supervisor:

```
                 altspotter
        ┌────────────┴────────────┐
   PostgreSQL (public,      NATS (bar.>, md.*, evt.universe.>)
    via postgres_fdw)              │  read-only
        │  read-only              │
        └────────────┬────────────┘
                     ▼
            ┌──────────────────┐
            │  alttech-service │
            ├──────────────────┤
            │ feature          │──┐
            │ microstructure   │  │ each also drives
            │ structure        │  │ its per-venue sweep
            │ regime           │──┘
            │ macro            │ ◀── AltMacro (read-only external series)
            │ cross_venue      │
            └────────┬─────────┘
                     ▼
   PostgreSQL (alttech's own database, NATS (alttech.>)
   `alttech` schema)
```

Why one process rather than six: the modules share the pair universe, the
connection pool and the NATS connection, and they must agree on what "this bar"
means. A single process makes that agreement a matter of shared memory rather
than of distributed coordination, and makes readiness one honest signal.

## Module boundaries

Each module owns exactly one kind of output and never writes another module's
tables.

| module | reads | writes |
|---|---|---|
| `feature` | `market_candles_1m`, `market_candles_agg`, JetStream `bar.>` | `venue_feature_vectors`, `venue_feature_projection`, `module_checkpoints`, `analytics_computation_runs` |
| `microstructure` | Core NATS `md.bitget.*.trade`, `market_orderbook_snapshots`, 1m aggregates | `venue_microstructure_vectors`, `module_checkpoints` |
| `structure` | `market_candles_agg` | `venue_structure_zones`, `structure_zone_sources`, `structure_snapshots`, `structure_zone_events` |
| `regime` | `venue_feature_projection`, `venue_feature_vectors`, `market_candles_agg` | `venue_regimes`, `regime_events` |
| `cross_venue` | `market_candles_1m`, 1m aggregates, `exchange_data_quality`, `exchange_clock_offsets`, `venue_regimes`, `venue_structure_zones` | `cross_venue_*`, `venue_alignment_state`, `global_regimes`, `consensus_structure_zones`, `bitget_projected_zones` |

| `macro` | `altmacro_ref` external price/reference/release series | `macro_feature_vectors` (40 values, GLOBAL scope) |

The primary venue gets the full analytics; the reference venues get a lighter
per-venue set produced by sweeps that the corresponding module drives:

- `feature` and `microstructure` sweep the **three reference venues** — the
  primary venue is already covered by their full engines;
- `structure` and `regime` sweep **all four venues** on the 1m grid, because the
  cross-venue consensus needs every venue's zone and regime on the same grid.

That split is why nothing is computed twice.

## Data flow

**Feature module.** The only latency-critical path. A closed bar arrives on the
JetStream durable; PostgreSQL is the ordering barrier, so the module waits until
altspotter's own writer has committed that exact bar and revision before
computing. The vector is written first and the message acknowledged second, so a
crash re-delivers rather than loses. On restart the trailing warm-up window is
replayed from PostgreSQL, so the in-memory engine is rebuilt rather than
restarted cold and the first bars after a restart are not falsely `WARMING`.

**The periodic modules** run bounded periodic sweeps against PostgreSQL rather
than per-event work. Analytics over a closed bar gain nothing from being
computed twice inside the same bar, and a sweep bounds the load regardless of
how active the market is.

## Failure semantics

Three levels, deliberately different:

1. **Transient** — a database error, a NATS reconnect, one venue briefly
   unavailable. The module logs, backs off and continues. A missing reference
   venue degrades the cross-venue *quality status*; it never stops anything.
2. **Module-fatal** — a schema or contract integrity breach: a table AltTech
   must write does not exist, or a frozen feature set no longer matches the
   engine that produced it. The module marks itself failed, which requests a
   coordinated process shutdown. Publishing analytics that silently mean
   something different is worse than publishing none.
3. **Process-fatal** — start-up validation fails, or the watchdog stops being
   fed. systemd restarts the process.

Readiness is reported to systemd only after **all six** modules have validated
their schemas and bound their subscriptions. "Started" therefore means "able to
produce output", not "the threads exist".

Liveness is two-layered: the main loop pings systemd's watchdog, but only while
no module has latched a failure *and* every module is still beaconing from its
own steady loop. A worker that wedges without crashing therefore also triggers a
restart.

## Idempotency and revisions

Every write is an UPSERT keyed on the natural key of the analytic — pair, venue,
timeframe, bar and feature-set version. Recomputing a bar overwrites it; there
is no way to accumulate duplicates.

altspotter revises candles when it repairs history. Analytics carry the
`bar_revision` they were computed from, so a consumer can see that a vector came
from a superseded bar, and the feature module rewrites affected vectors when the
revision changes.

Published events carry a deterministic message id derived from that same natural
key, so JetStream deduplicates a republished window.

## Ownership boundaries

AltTech owns the `alttech` PostgreSQL schema, the `ALTTECH_ANALYTICS` JetStream
stream and its own durable consumer on altspotter's `ALTSPOTTER_BARS`. It owns
nothing else:

- it never creates, alters or drops anything in `public`;
- it never mutates an altspotter stream or attaches to an altspotter durable;
- it holds no exchange credentials and opens no exchange connection.

`alttech-nats-setup` is read-only by default; with an explicit `--apply` it can
create — never update or delete — the AltTech stream and durable. Neither
service start-up nor the systemd target ever runs it in apply mode.

## Precision

Money read from altspotter stays `NUMERIC` on the read side and becomes `double`
only inside the analytics domain, where the output is a derived statistic rather
than a settlement figure. Wire decimals use an exact mantissa/scale
representation. No price is ever reconstructed from a `float`.

## Macro context

The macro module reads external series through `altmacro_ref`, computes measured
returns/volatility/rate/spread features and preserves per-input validity and
availability. It publishes context, not a predicted crypto direction. Required
`macro.history_start` follows configured source coverage; there is no built-in
historical date. Bounded startup history chunks and trailing live recomputation
share the source-time contract. See [configuration](CONFIGURATION.md).
