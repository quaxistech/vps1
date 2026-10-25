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

## Topology and authority

Public venues feed AltSpotter, then AltTech analytics and AltML forecasts.
AltMacro separately supplies external/reference series to AltTech's macro
module and applicable point-in-time contexts. Strategy reads coherent inputs
and publishes market setups; Quality validates and forwards technically valid
proposals; Gateway journals and serves the external protocol-2/schema-3 boundary.
A proposal, forecast or diagnostic quality score is not execution approval.
VPS2 independently owns admission, quantity/leverage, orders and protection.

PostgreSQL schemas, NATS/protobuf contracts and immutable context/availability
metadata are owned by each producer. Downstream consumers cannot write upstream
state or backdate future data. Each service README links its configuration,
storage, recovery and security contract. [Cross-service contracts](altspotter/docs/vps1/CONTRACTS.md)
and [temporal rules](altspotter/docs/vps1/TEMPORAL_MODEL.md) define shared semantics.
[Source limitations](docs/source-contract-limitations.md) records the two bounded
static discrepancies found during this audit.

Runtime is **NOT VERIFIED DURING DOCUMENTATION-ONLY AUDIT** (2026-10-25).
The source baseline reviewed was `53c76051ed11964de1bf49f0b12e55cb81fcf4bc`;
no production host was contacted. The published signal architecture remains
frozen for downstream VPS2 changes; independent runtime changes require the
owner authorization below. No new predictive or profitability claim is made.

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
