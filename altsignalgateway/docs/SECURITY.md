# Security

AltSignalGateway is the authenticated external delivery boundary for final
trade proposals. An unauthorized reader could disclose the feed; an
unauthorized writer could inject a message into an execution consumer.

## TLS

**There is no plaintext mode.** `tls.enabled` does not exist as a setting. A
switch that turns transport security off is a switch someone eventually leaves
off, and a missing certificate stops the service at startup with a message
naming the file.

| setting | default | note |
|---|---|---|
| `min_version` | `1.3` | `1.2` is configurable for a consumer that cannot do 1.3 |
| SSLv2/v3, TLS 1.0/1.1 | disabled unconditionally | not configurable |
| cipher selection | server preference | otherwise a client steers to the weakest mutually supported suite |
| TLS 1.2 suites (when enabled) | ECDHE + AEAD only | forward secrecy and authenticated encryption; without both, a recorded session is decryptable later from a leaked key |
| session tickets | disabled | a second place a session key can be recovered from, for no benefit on one long-lived connection |
| private key permissions | **checked at startup** | world-readable ⇒ refuse to start |

Local development and the integration tests use a self-signed certificate from
`scripts/gen-tls-cert.sh`, with the client pinning that exact certificate as its
CA. Self-signed **with an explicitly pinned trust anchor is stronger than a
public CA**, not weaker: only that one key can serve the endpoint. A public CA
is the right answer only when unknown clients must trust the endpoint without
prior arrangement, which is not this situation.

The reference client has **no `--insecure` flag**. A client that skips
verification is a client that can be handed someone else's trades.

### Optional mutual TLS

`tls.require_client_certificate` adds certificate verification **on top of** the
HMAC handshake, not instead of it. It is off by default and turned on once the
external consumer has a certificate.

`verify_fail_if_no_peer_cert` is set alongside `verify_peer`; without it,
`verify_peer` accepts a client that presents no certificate at all. A
`client_ca_file` is required when the flag is on — verifying against an empty
trust store accepts nothing and would silently lock every client out while
looking like an improvement.

## Authentication — ASGW-AUTH-v1

HMAC-SHA256 challenge-response over TLS.

**Why not a static bearer token.** It travels on every connection and is
replayable forever by anyone who ever saw one — a log line, a proxy, a core
dump.

**Why not mTLS as the identity.** A certificate identifies a certificate, not a
protocol client, and rotating a compromised key means re-issuing a certificate
on a box that may be unattended. It is a good *second* factor, which is how it
is offered.

**Why a signed handshake.** It binds the proof to one connection. The server
issues a single-use 32-byte CSPRNG nonce; the client signs it together with the
session id and its own nonce; the signature is worthless anywhere else.

The canonical string is newline-separated with every component in fixed order,
so two different field splits can never produce the same bytes to sign —
`("ab","c")` and `("a","bc")` would otherwise collide and a signature for one
identity would authenticate another. `tests/unit/test_auth.cpp` asserts it.

### Replay protection, layered

1. **`server_nonce`** — 32 CSPRNG bytes, generated per connection, accepted
   exactly once, and zeroed after use. A captured `Auth` frame cannot be
   replayed onto a new connection.
2. **`session_id`** — random per connection, and inside the signed string. A
   predictable session id would let a captured signature be replayed onto a
   predicted session; the id is 16 CSPRNG bytes.
3. **`timestamp_ms`** — must be within `max_clock_skew_sec` (60 s) of the server
   clock, which bounds how long a signature is even syntactically current.
4. **`client_nonce`** — ≥16 bytes, so the client contributes entropy of its own.
5. **The state machine** — a second `HELLO` or `AUTH` on a live connection is
   `ERR_PROTOCOL_VIOLATION`, so a captured frame cannot be replayed against a
   session whose nonce has already been consumed.

### Comparison and error reporting

Signature comparison is `CRYPTO_memcmp` — constant time. A byte-wise early exit
leaks the position of the first mismatch to a patient attacker.

A failure returns `ERR_AUTH_FAILED` with no further detail. The **server log**
records which check failed (`unknown_key_id`, `client_id_mismatch`,
`nonce_mismatch`, `timestamp_skew`, `bad_signature`, `missing_capability`).
Telling a caller *why* its signature was rejected is a free oracle for refining
an attack.

### Brute-force bound

`auth_failure_threshold` failures from one peer within
`auth_failure_window_sec` trigger a lock-out for `auth_lockout_sec` (5/60s/300s
by default), applied **before any cryptography runs** on the next connection.

It is a sliding window, not a lifetime counter: a client that fails once a day
for a fortnight is not under attack, and locking it out on the fifteenth day
would be the service inventing an outage. A successful authentication clears the
counter.

## Authorization

After authentication a session has an identity, a protocol version, and a
capability set:

| capability | grants |
|---|---|
| `receive` | receive signals |
| `ack` | acknowledge (advances **this client's** cursor and nothing else) |
| `resume` | request a replay position |

There is no fourth capability because there is no frame in the protocol that
does anything else. A client **cannot**: reach PostgreSQL, reach NATS, reach any
other VPS1 service, alter a signal, alter another client's cursor, or write any
state into VPS1 beyond its own position.

Capabilities are stated explicitly per credential; a credential that lists none
is a configuration error and refuses to load. A client with `receive` but no
`ack` is a read-only observer whose watching can never advance the executor's
cursor.

## Secrets

| secret | where it lives | mode |
|---|---|---|
| WebSocket client HMAC keys | `/etc/altsignalgateway/common.env`, referenced as `${VAR}` from `clients.yaml` | 0640 root:altsignalgateway |
| PostgreSQL DSN | `common.env` | 0640 |
| NATS URL with password | `common.env` | 0640 |
| TLS private key | `/etc/altsignalgateway/tls/server.key` | 0640, **checked at startup** |

Enforced, not merely documented:

* `load_credentials` refuses a world-readable credentials file.
* `WsServer::prepare` refuses a world-accessible private key.
* An unset `${VAR}` is a startup error, never an empty default — a service that
  starts with an empty client secret has no authentication.
* Keys must be ≥32 bytes of hex; shorter is refused.
* Two clients cannot share a `key_id` — it is what an `Auth` frame selects, and
  a shared one would make the identity behind a signature ambiguous.
* `LimitCORE=0` in the systemd hardening: a core dump of this process would
  contain the HMAC keys and TLS session state in plaintext.
* No secret is ever logged. The log records a `key_id`, never a key.
* `tests/shell/test_release_contract.sh` scans the shipped tree for literal
  secrets, private keys and non-`${VAR}` `secret_hex` values.

**Key rotation.** Add a second entry for the same `client_id` with a new
`key_id`, let the consumer switch, remove the old entry. Both are valid during
the overlap; neither side needs a coordinated restart.

## Input validation

| input | bound |
|---|---|
| WebSocket message | `max_message_bytes` (1 MiB) — refused by the transport *before* buffering |
| inbound FINAL signal size | the same bound, applied to the canonical payload at ingest: a signal that could not be transmitted is refused as `PAYLOAD_TOO_LARGE` rather than journalled. Every free-text field on the upstream contract is unbounded, so this is where the boundary imposes a limit. |
| frame type | binary only; a text frame is `ERR_MALFORMED_FRAME` |
| frame content | must parse as `ClientFrame`; a `oneof` with no body is refused |
| frame order | the state machine; anything unexpected closes the connection |
| `client_id` | 1..128 bytes |
| `client_nonce` | ≥16 bytes |
| `Resume.mode` | must be explicit; `UNSPECIFIED` is refused |
| `Resume.last_acked_sequence` | ≥0 and ≤ journal head, or refused |
| `Ack` | must match the message the server sent |
| inbound FINAL signal | the full contract validator; failures land in `rejected_messages` and the NATS message is terminated |

A message that fails contract validation is **terminated**, not redelivered: it
will never become valid, and redelivering it forever would wedge the consumer
behind it.

## Connection limits

| limit | effect |
|---|---|
| `max_clients` (8) | an **unauthenticated** connection already occupies a slot — this bounds the cheapest denial of service, not just the number of consumers |
| `handshake_timeout_sec` (10) | a connection that opens a socket and says nothing does not keep a file descriptor |
| `resume_timeout_sec` (30) | an authenticated session that never resumes is closed |
| `accept_backlog` (16) | |
| `TasksMax=24` (systemd) | a thread explosion here would be a bug |
| `MemoryMax=256M` (systemd) | |

A failed `accept` never ends the accept loop: a transient `EMFILE` or a client
that vanished between SYN and accept would otherwise take the endpoint down
until the next restart.

## Database privileges

The service role owns exactly the `altsignalgateway` schema and has **`SELECT`
only** on five upstream tables:

```
altstrategy.decision_context_snapshots
public.canonical_pairs
public.canonical_pair_exchange_map
public.exchange_instruments
public.exchanges
public.instrument_spec_versions
```

It cannot write to another service's schema, cannot read AltStrategy's model,
outcome or training tables, and has no access to the market-data archive at all.
`tests/shell/test_schema_invariants.sh` asserts `db-bootstrap.sh` grants no
write privilege outside its own schema.

Schema names from the configuration are interpolated into SQL and are therefore
validated as `[A-Za-z0-9_]+` at load time. The configuration is root-owned, but
a service that builds SQL from a string must still be provably injection-free.

## NATS privileges

```
subscribe   signalquality.final.>     the FINAL signals it journals
            _INBOX.>
publish     $JS.API.>, $JS.ACK.>      bind its durable and acknowledge
            _INBOX.>
```

No publish permission on any subject that carries data — this service produces
nothing on the bus. It cannot read `signal.>` (AltStrategy's proposals),
`alttech.>`, `prediction.>` or any market-data subject.

## Filesystem

```
/opt/altsignalgateway/     binary + docs, root-owned, read-only, immutable per release
/etc/altsignalgateway/             config, 0750 root:altsignalgateway
  ├── altsignalgateway.yaml        0640
  ├── common.env                   0640   secrets
  ├── clients.yaml                 0640   secrets (by reference)
  └── tls/                         0750
        ├── server.crt             0644
        └── server.key             0640   permission-checked at startup
/var/lib/altsignalgateway/         runtime state, 0750, the ONLY writable path
```

Runtime state is deliberately not next to the executable: `/opt` is made
read-only by the deploy and by `ProtectSystem=strict`.

## Sandbox

`NoNewPrivileges`, `CapabilityBoundingSet=` (empty — nothing here needs to bind
below 1024), `ProtectSystem=strict`, `ProtectHome`, `ProtectProc=invisible`,
`PrivateTmp`, `PrivateDevices`, `RestrictAddressFamilies=AF_UNIX AF_INET
AF_INET6`, `RestrictNamespaces`, `MemoryDenyWriteExecute`, `LockPersonality`,
`SystemCallArchitectures=native`, a `SystemCallFilter` denylist, `UMask=0027`,
`LimitCORE=0`.

## Network exposure

The listener binds `0.0.0.0:9443` because this **is** the external boundary.
Reachability is a firewall decision, not a bind-address decision.

Firewall rules determine which clients can reach the listener. Restrict access
to the intended consumer networks:

```sh
ufw allow from <consumer-address> to any port 9443 proto tcp
```

Prefer source-restricted rules over a blanket port opening.

## Logged, and never logged

Logged: signal journalled, delivered, acknowledged; client connected,
authenticated, disconnected; resume requested and completed; signal expired;
protocol error; authentication failure with the *reason* and the `key_id`;
journal recovery; integrity violation; payload hash mismatch.

Never logged: any secret, any HMAC key, any signature, any nonce, any TLS key
material, any credentials-file content beyond `client_id` and `key_id`.
