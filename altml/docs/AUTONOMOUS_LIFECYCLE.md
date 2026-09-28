# Autonomous lifecycle

Normal operation uses one command:

```sh
systemctl start altml.service
# or: altml run --config /etc/altml/altml.yaml
```

The service schedules training from the `training` configuration:

- **Bootstrap:** train when `training.min_rows` of joined feature and label
  history are available and `training.bootstrap_if_no_active` is enabled.
- **Steady state:** train every `training.interval_hours` within the configured
  `training.start_hour_utc` window.
- **Retry:** wait `training.retry_minutes` after resource or history deferrals.
  Rejected cycles use an exponentially increasing delay capped at the regular
  training interval; the rejection streak is restored from the model registry.
- **Resource guard:** defer training when load or free memory crosses the
  configured limits. Live inference retains priority.

`altml train [--force]` runs one manual cycle for diagnostics and CI. It is not
required during normal operation.

## Storage lifecycle

The storage guard measures database usage, applies per-table retention policies,
prunes retired model artifacts, and records each pass. When usage reaches the
configured hard limit, it reduces retention to each policy's minimum. See
[Storage and retention](STORAGE_BUDGET.md).

## CLI tools

`altml nats-setup` provisions AltML-owned stream resources.
`altml model-validate` verifies a stored model artifact against its recorded
predictions. `altml replay` reproduces stored predictions through the inference
pipeline.
