# Configuration

Two files plus an environment file:

| file | contents |
|---|---|
| `/etc/alttech/common.env` | secrets and connection strings |
| `/etc/alttech/common.yaml` | shared runtime parameters, `${VAR}` expanded from the env file |
| `/etc/alttech/alttech.yaml` | the service configuration |

`${VAR}` placeholders are expanded from the environment. A missing variable is a
hard error — a secret must be present explicitly, never silently defaulted.

Validate before starting, which is also what the unit does as `ExecStartPre`:

```sh
alttech-config-check --file /etc/alttech/common.yaml  --kind common
alttech-config-check --file /etc/alttech/alttech.yaml --kind service
```

## common.env

| variable | notes |
|---|---|
| `ALTTECH_ENVIRONMENT` | free-form label carried in logs |
| `ALTTECH_LOG_LEVEL` | `trace`…`critical` |
| `ALTTECH_INSTANCE_ID` | identifies this process in logs |
| `ALTTECH_NATS_URL` | `nats://user:password@127.0.0.1:4222` |
| `ALTTECH_POSTGRES_DSN` | runtime DSN for AltTech's own database; needs `SELECT` on the `public` foreign tables imported via `postgres_fdw` and full rights on `alttech` |
| `ALTTECH_NATS_INPUT_STREAM` | altspotter's bar stream, default `ALTSPOTTER_BARS` |
| `ALTTECH_NATS_OUTPUT_STREAM` | `ALTTECH_ANALYTICS` |
| `ALTTECH_NATS_DURABLE` | AltTech's own durable, default `alttech-bars` |
| `ALTTECH_MIGRATION_DSN` | optional schema-only DSN; falls back to the runtime DSN |

The stream and durable names are shared by the service and by
`alttech-nats-setup`, so the topology that is checked can never drift from the
topology the service actually uses.

## alttech.yaml

### Process

| key | default | meaning |
|---|---|---|
| `service` | — | must be `alttech` |
| `postgres_pool_size` | 8 | shared by every module; must be at least `feature.workers + 4`, because the feature workers, the feature consumer and the other four modules can all hold a lease at once |
| `postgres_statement_timeout_ms` | 120000 | server-side time limit for pooled queries. Set it above the expected duration of a legitimate backfill chunk read. `0` disables; otherwise 1000–600000 |

### Venue identity

| key | default | meaning |
|---|---|---|
| `primary_exchange` | `bitget` | the single trading venue; validated |
| `reference_exchanges` | `binance_usdm, bybit, okx` | exactly these three; spot is rejected |

The pair universe is **not** configured. It comes from altspotter's global
observation universe — the intersection of all four venues.

### Storage

| key | example value | meaning |
|---|---|---|
| `storage.analytics_soft_limit_bytes` | 2.9 GB | AltTech's own allocation |
| `storage.total_database_hard_limit_bytes` | 3 GB | ceiling for the whole shared database |
| `storage.analytics_warning_ratio` | 0.80 | reported |
| `storage.analytics_cleanup_ratio` | 0.90 | budget pressure begins |
| `storage.analytics_critical_ratio` | 0.95 | reported |

Ratios must satisfy `0 < warning < cleanup < critical < 1`, and the soft limit
must be below the hard limit; both are validated. See [STORAGE.md](STORAGE.md).

### `modules.feature`

| key | default | meaning |
|---|---|---|
| `bar_stream`, `bar_consumer` | from env | JetStream stream and durable |
| `workers` | 1 | backfill/recompute workers, 1–16 |
| `backfill_chunk` | 5000 | candles per read, 1–100000 |
| `write_batch` | 400 | vectors per UPSERT, 1–2000 |
| `publish_core_vectors` | true | publish live vectors to NATS |
| `timeframes` | `1m,5m,15m,30m,1h,4h,1d` | a timeframe can be switched off entirely |
| `backfill_days` | 2 | how far back the replay reaches, for every timeframe. **A storage decision**: analytics can only be written where a partition exists, so it must stay inside the retention window |
| `task_retry_backoff_min_ms` / `_max_ms` | 1000 / 30000 | exponential backoff with jitter |
| `cycle_interval_sec`, `venue_lookback_min`, `venue_window_len` | 30 / 180 / 220 | the reference-venue sweep this module drives |

### `modules.microstructure`

| key | default | meaning |
|---|---|---|
| `tick_interval_ms` | 500 | window close cadence, 100–5000 |
| `book_poll_page_size` | 50000 | book snapshot rows per page |
| `book_poll_max_pages_per_tick` | 4 | bounds one tick's read |
| `book_poll_overlap_ms` | 2000 | cursor overlap, deduplicated by `(instrument, timestamp)` |
| `store_batch_size` | 2000 | vectors per write |
| `persist_windows` | `15s,30s,1m` | which of `1s,5s,15s,30s,1m` are written to the database. All are published live regardless |

### `modules.structure`

| key | default |
|---|---|
| `cycle_interval_sec` | 60 |
| `venue_lookback_min`, `venue_window_len` | 180, 220 |

### `modules.regime`

| key | default | meaning |
|---|---|---|
| `cycle_interval_sec` | 30 | |
| `backfill_days` | 0 | classify the retained feature history at start-up; 0 disables |
| `thresholds.adx_trending` | 25.0 | Wilder: an established trend |
| `thresholds.low_liquidity_volume` | 0.15 | volume axis, about −2σ |
| `thresholds.chaotic_efficiency` | 0.12 | Kaufman efficiency ratio |
| `thresholds.chaotic_volatility` | 0.85 | volatility percentile |
| `thresholds.expansion_volatility` | 0.90 | volatility percentile |
| `thresholds.breakout_volume_z` | 1.5 | volume z-score confirming an expansion |
| `thresholds.compression_volatility` | 0.12 | volatility percentile |

Validated for range and ordering (`compression < chaotic ≤ expansion`). The
defaults are the conventional reading of each axis; see
[ANALYTICS.md](ANALYTICS.md) for what each axis is.

### `cross_venue`

| key | default | meaning |
|---|---|---|
| `enabled` | true | when false the module stays idle but reports ready |
| `cycle_interval_sec` | 30 | sweep cadence |
| `lookback_min` | 180 | how far back a missed bar is still computed |
| `lead_lag_window` | 30 | 1m samples in the cross-correlation |
| `lead_lag_max_lag` | 5 | maximum lag searched, in minutes |
| `min_consensus_venues` | 2 | venues required for a consensus zone |
| `zone_stale_after_sec` | 1800 | unconfirmed consensus zones expire |
| `alignment.bar_watermark_timeout_ms` | 4000 | how long to wait for a slow venue after bar close |
| `alignment.max_venue_data_age_ms` | 180000 | how far behind its own market data a venue may be and still count. Measured against the 1m grid, so three bar intervals |
| `alignment.max_cross_venue_skew_ms` | 2500 | above this the window is not `STRICT_VALID` |
| `weighting.method` | `quality_weighted_median` | or `quality_weighted_mean`, `trimmed_weighted_mean` |

The alignment bounds are the values most worth retuning after deployment.
They are conservative starting points, not measurements; the skew a deployment
actually sees is reported by `alttech-analytics alignment-inspect`, and the
bounds should be set from that.

## What is deliberately not configurable

- **The pair universe** — it comes from altspotter, so the two services can
  never disagree about which pairs exist.
- **The feature catalog** — a feature set is frozen once written to the
  database; changing it is a new set version, not a setting.
- **Which venue is primary** — validated as `bitget`. The analytics are built
  around one trading venue and three references; making that a free choice would
  imply a symmetry the design does not have.
