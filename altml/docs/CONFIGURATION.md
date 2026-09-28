# Configuration

One file drives every mode: `/etc/altml/altml.yaml` (example:
[../config/altml.example.yaml](../config/altml.example.yaml)). `${VAR}`
placeholders are expanded from the environment (`/etc/altml/common.env` under
systemd); a missing variable is a startup error.

## Sections

```
<root>              environment, log_level, timezone, instance_id, nats_url,
                    postgres_dsn, shutdown_timeout_sec
project             the adapter: exchange, input/output/control streams+subjects,
                    dataset/model roots
inference           feature-vector freshness bound, connection pool size
label_builder       labeling cycle interval, batch size, fee model version,
                    revision-audit interval
training            the native in-process trainer: bootstrap_if_no_active,
                    auto_promote, cadence (interval_hours/start_hour_utc/retry),
                    rolling window (lookback_days/sampling/max_rows/min_rows),
                    walk-forward (folds/embargo/validation_fraction), LightGBM
                    hyperparameters, economic gates (fees/slippage/PF/drawdown),
                    resource guards (threads/time budget/matrix budget/load/mem)
model_registry      poll interval
storage             database size budget (pg_soft_budget_gb/pg_hard_limit_gb),
                    check interval, retired-model artifact retention count
```

The loader stores the whole document; each component reads only its own
section, so adding a section cannot break `altml run`. Only Bitget is a
supported `project.exchange` today; any other value is rejected loudly. See
[INTEGRATION.md](INTEGRATION.md) for the project adapter and
[STORAGE_BUDGET.md](STORAGE_BUDGET.md) for `storage:`.

## Secrets

`postgres_dsn` and `nats_url` are `${ALTML_POSTGRES_DSN}` / `${ALTML_NATS_URL}`,
resolved from `/etc/altml/common.env` (root:altml, 0640). Never put a
credential directly in `altml.yaml`, the repository, or a release archive.
`scripts/db-bootstrap.sh` takes its own connection parameters
(`ALTML_DB_*`, `ALTSPOTTER_FDW_*`, `ALTTECH_FDW_*`) from the environment too —
see [config/altml.env.example](../config/altml.env.example).
