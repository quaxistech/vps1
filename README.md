# VPS1

Monorepo for the services that make up the VPS1 market-intelligence system.
Each service remains a self-contained project with its own build, configuration,
schema and deployment files.

## Services

| directory | responsibility |
|---|---|
| [`altspotter/`](altspotter/README.md) | market-data collection and history |
| [`alttech/`](alttech/README.md) | market analytics and feature generation |
| [`altmacro/`](altmacro/README.md) | external macro and reference data |
| [`altml/`](altml/README.md) | market forecasts and model lifecycle |
| [`altstrategy/`](altstrategy/README.md) | setup generation and trade proposals |
| [`altsignalquality/`](altsignalquality/README.md) | proposal validity and quality metadata |
| [`altsignalgateway/`](altsignalgateway/README.md) | durable final-signal delivery to clients |

See [`altspotter/docs/vps1/VPS1_ARCHITECTURE.md`](altspotter/docs/vps1/VPS1_ARCHITECTURE.md)
for the cross-service architecture. Build and deployment instructions live in
each service directory.

## Governance

VPS1 is a production system, not a frozen one. Every change to what runs --
a deployment, a migration, a configuration or lifecycle change, a model
promotion, a restart -- is made only with the owner's explicit approval.
Read-only inspection, offline research and replay do not change what runs, and
research is how changes earn that approval. Builds happen on the development
workstation, never on the host.

Build outputs and release archives are excluded. Per-host `vps1.md` target
files and the machine-local `altsecrets/` directory are also ignored; keep
credentials and workstation notes there, outside Git history.
