# Security

## Surface

altspotter is a **public market-data** plane: outbound TLS to public exchange
REST/WS endpoints only. It uses **no exchange API keys** and touches no
private/account endpoints. PostgreSQL and NATS bind to `127.0.0.1` only; nothing
is exposed to the internet. Consumers run on the same host or via SSH tunnel.

## Transport

- TLS certificate + hostname verification is mandatory on every REST and WS
  connection (SNI + `SSL_set1_host`, `verify_peer`); TLS ≤ 1.1 disabled.
- Secrets (DB DSN, NATS credentials) come from `/etc/altspotter/*` (root:altspotter, 0640) and
  env; they are never logged and never in git. Config `${VAR}` expansion fails
  closed on a missing variable.

## systemd hardening

The unit keeps the baseline sandbox: `NoNewPrivileges`, `ProtectSystem=strict`,
`ProtectHome`, `PrivateTmp`, empty `CapabilityBoundingSet`, restricted address
families/namespaces, `SystemCallFilter` denylist, `LockPersonality`. Resource
limits (`MemoryMax`, `TasksMax`, `LimitNOFILE`) bound the process.

## Data integrity

- Exact fixed-point decimals — no float money; exchange decimal strings parsed
  digit-by-digit.
- Idempotent natural keys + JetStream dedup ids — replay/reconnect never creates
  duplicates.
- Order books validated (sequence/checksum) before publishing as VALID; poison
  messages are journalled (`rejected_market_events`) and TERMed, never silently
  dropped or allowed to wedge the stream.

## Untrusted input

All venue payloads are treated as untrusted: JSON parsing is non-throwing/guarded,
frame handlers are exception-isolated (a malformed frame drops that frame, never
the io thread), and SQL is parameterized or identifier-quoted.
