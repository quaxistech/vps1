# Security

## Trust boundary

AltStrategy consumes market data and publishes proposals. It holds no exchange
credentials, opens no exchange connections, and emits no orders. Execution and
risk services decide whether and how to act on a proposal.

## Data access

- Reads upstream NATS events and AltSpotter market history through a read-only
  PostgreSQL role.
- Writes only the `altstrategy` schema and its own NATS stream.
- External signal delivery is provided by AltSignalGateway, with its own
  transport and client-authentication controls.

## Secrets

Database and NATS credentials are supplied through `/etc/altstrategy/` runtime
configuration. Keep secrets out of source files, protobuf messages, command
arguments, and logs.

## Process isolation

The systemd service runs as the unprivileged `altstrategy` account with a
restricted writable data directory and service hardening. The service has no
exchange network client and no trading authority.

## Data integrity

Proposal and context records preserve their source identities, revisions, and
point-in-time timestamps. Deterministic identifiers and database uniqueness
constraints support idempotent processing of redelivered messages.
