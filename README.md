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

Build outputs, release archives, per-host `vps1.md` target files, and local
credential files are excluded from this repository.
