# Operations

## Service

AltML runs as one systemd service. Its training schedule is managed inside the
process; it has no separate training timer or one-shot service.

```sh
systemctl status altml.service
systemctl start altml.service
systemctl stop altml.service
journalctl -u altml.service -f
```

The service configuration and environment file are under `/etc/altml/`.
Model artifacts are stored under `/var/lib/altml/models/`.

## State and manual commands

`systemctl status altml.service` reports service readiness. `altml train` runs
one training cycle and `altml replay` reproduces stored predictions. Routine
training is scheduled automatically; see [Autonomous lifecycle](AUTONOMOUS_LIFECYCLE.md).
`altml model-validate` checks a model artifact against recorded predictions.

`altml nats-setup` provisions AltML-owned JetStream resources. The service also
performs this setup during startup.

## Deployment and configuration

Build and install the service with the scripts under `deploy/` and `scripts/`.
Database roles, read-only source access, NATS permissions, and configuration
files are described in [Database](DATABASE.md), [NATS](NATS.md),
[Configuration](CONFIGURATION.md), and the repository [README](../README.md).

Credentials belong in `/etc/altml/common.env`. Keep them out of source files,
command arguments, and logs.
