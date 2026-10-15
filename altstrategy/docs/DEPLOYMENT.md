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
