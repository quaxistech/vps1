# Security & boundary

## Hard boundary

AltML is a forecasting core, not a trader. It **never**:

- connects directly to an exchange;
- holds private exchange credentials;
- publishes a trade signal;
- executes orders or manages positions;
- sets a stop-loss/take-profit.

`altml.v1.PredictionV1` has no order/trade-signal payload — enforced by
`tests/unit/test_wire_contract.cpp`. AltML reads AltSpotter's and AltTech's
tables read-only, through `postgres_fdw`, and writes only its own `altml`
schema — enforced by the reader roles `scripts/db-bootstrap.sh` creates
(`GRANT SELECT` only, on a named, minimal column/table list).

## Secrets & configuration

Secrets are referenced as `${ALTML_*}` and resolved from
`/etc/altml/common.env` (root:altml, 0640) under systemd; a missing variable
is a startup error, never an empty default. No credential ever lives in the
repository, a release archive, or a model artifact. Database/NATS passwords
must be generated as hex (`openssl rand -hex N`), never base64 — a base64
password embedded in a `postgresql://` DSN can contain `/` or `+` and break
URI parsing.

## Training

Training runs **inside** the service binary (LightGBM C API) — no
interpreter, no subprocess, no runtime dependency download, nothing to
shell-inject. The attack/supply-chain surface is the pinned C++ dependency
set only. Resource bounds (threads, memory estimate, wall-clock budget,
load/memory guards) keep a training cycle from starving live inference; the
systemd sandbox and `MemoryMax` cap the whole process.

## Artifact trust

Registry paths are data, not authority: the model loader resolves symlinks
and rejects any artifact outside the configured `model_root`, then verifies
the booster checksum and feature count before load. See
[ARTIFACTS.md](ARTIFACTS.md).

## Deployment

Native systemd, no Docker. One installed tree under `/opt/altml/` (no release
history, no `current` symlink, no backups);
runtime data under `/var/lib/altml`; configuration under `/etc/altml`. The
systemd hardening drop-in (`altml-service-hardening.conf`) applies
`ProtectSystem=strict`, `NoNewPrivileges`, a restricted syscall filter and a
minimal capability set to every AltML unit. See [OPERATIONS.md](OPERATIONS.md).
