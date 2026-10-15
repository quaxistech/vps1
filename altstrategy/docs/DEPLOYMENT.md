# Deployment

AltStrategy is built on the build host and installed as a native systemd
service. The target host needs the runtime libraries and PostgreSQL/NATS access,
not the source tree or build toolchain.

## Build and publish

```sh
cmake --preset release
cmake --build --preset release
scripts/deploy.sh
```

The deploy script stages and verifies the release, installs it under
`/opt/altstrategy/`, and leaves a stopped service stopped. It does not apply
database migrations.

## Host setup

Provision the service account and data directory with `scripts/provision.sh`.
Install the database schema by applying the ordered files under
`db/migrations/` to the AltStrategy-owned schema. Configure the service under
`/etc/altstrategy/`, then provision `ALTSTRATEGY_DECISIONS` with
`altstrategy-nats-init`.

The service reads PostgreSQL and NATS credentials from its environment file.
Credentials stay outside the source and release archive.

## Start and update

```sh
systemctl enable --now altstrategy.service
```

For a release that changes the database schema, apply the corresponding
migration before starting the new binary. The service runs as one systemd unit
and stores its runtime archive and exports under `/var/lib/altstrategy/`.

## The signal-quality release (forward only)

Rehearsed end to end on 2026-10-15 with the real binaries, plain and under
ASan/UBSan, on a throwaway PostgreSQL 16 and nats-server reproducing
production (0001..0010 in the ledger, the database's `search_path`, the
AltSpotter FDW server, a 2-minute duplicate window). Every step changes
production and needs explicit approval. Nothing in it deletes data: every new
column is nullable (NULL reads as version 1), and the operator SQL keeps what
it replaces.

| step | action | check before the next step |
|---|---|---|
| 1. schema | `altstrategy-db-migrate --dsn … --migrations db/migrations` | `SELECT max(version) FROM altstrategy.schema_migrations` is `0013`. A failure leaves the last good version recorded and nothing of the failed file: fix the cause and run it again. A further run applies nothing and verifies every checksum. |
| 2. price grid | as `altstrategy_app`: `psql -d altstrategy -f db/fdw/altspotter_reference.sql` | `SELECT count(*) FROM public.instrument_spec_versions` > 0. Without it every proposal carries `TICK_SIZE_DERIVED`, one more warning on every signal VPS2 counts. |
| 3. stream | `altstrategy-nats-init` | `nats stream info ALTSTRATEGY_DECISIONS`: duplicate window 24 h. AltSignalQuality requires at least its own window: satisfied. |
| 4. configuration | `learning.min_plan_geometry_version: 2`, `calibration.enabled: false` | read at the next start: `engine_configured` logs the version, an invalid value is `config_rejected` and a refused start. |
| 5. parameters | `db/operator/quality05_parameter_reset.sql` (recommended, below) | its NOTICE names the sets rolled back; one active set per strategy. |
| 6. binary | `scripts/deploy.sh` -- it restarts a running service | journal: `service_started`, `open_hypotheses_restored`, no `schema_not_migrated`, no `decision_not_archived`; the `publication_outbox` report shows nothing pending after a minute. |
| 7. AltSignalQuality | its release | evaluations resume; `MISSING_FEATURES` on a minority of evaluations, no longer on all. |

Steps 1–3 come before 6: the restart in `deploy.sh` brings up the new binary,
which refuses a schema older than 0013 (exit 1, retried by systemd every 5 s)
and would otherwise run without the price grid.

**Why the parameter reset.** On 2026-10-15, 15 of 16 strategies ran calibrated
sets: 31 of their 32 changed values are the `stop_atr` / `target_atr` multiples
the plan generator turns into the strategy's own stop and target, 23 of them
at their schema minimum and one at its maximum, moved there by a calibrator
that measured outcomes of plan geometry 1 (and, before it was keyed by
parameter set, outcomes the set had not produced). Under geometry 2 those
multiples set the levels VPS2's thesis layer reads as the plan's stop. The
only setup threshold moved is `book_imbalance_pressure.imbalance_min` (0.333 →
0.25). The reset is not required for a correct start; it returns the setups
to their reviewed defaults at the same cut-over as geometry 2, so the
learning epoch begins once. With `calibration.enabled: false` the sets then
stay fixed until a decision taken on outcomes of geometry 2.

There is no downgrade path: this release only moves forward.

### Deployed 2026-10-15

Built locally from commit `11760fa` (altstrategy archive sha256 `22de6752…`,
service binary `306c58fe…`; AltSignalQuality archive `160d4bb3…`, binary
`d1ce55ed…`); local gates altstrategy 202/202, AltSignalQuality 143/143.

- Steps 1–5: ledger 0010 → 0013 at 20:55:22 UTC (checksums equal the
  repository; a rerun applied nothing); 28 instrument-spec rows for 27 pairs;
  duplicate window 86 400 s, the rest of the stream unchanged; configuration
  sha256 `eee3355c…`; 15 calibrated sets rolled back, one active set per
  strategy.
- Step 6: altstrategy running since 22:25:13 UTC, `open_hypotheses_restored`
  36. Step 7: AltSignalQuality since 22:27:22 UTC.
- First 22 minutes: 48 proposals, all geometry 2 on the venue tick grid; no
  off-grid price, side violation or target under 5 bps; ML targets equal the
  side's own excursion (ratio 1.003 LONG, 0.993 SHORT). Outbox: 279 rows acked
  on the first attempt, none owed or dead-lettered. Gateway sequences
  27063–27078 contiguous and ACKED by VPS2. No error in either journal.
- `MISSING_FEATURES`: right after the restart every evaluation lacked the
  `htf_*` members (no higher-timeframe bar had closed yet); from the first
  30m close the count fell to 2–4, and the remaining reasons are real
  cross-venue gaps (`xv_rsi_dispersion`, `xv_volatility_agreement`; absent in
  11 % of the preceding 14 days).
- Not redeployed: AltML (behaviour-neutral refactor) and the Gateway
  (documentation only; its NATS client keeps the teardown race fixed here).
