# Consumer guide

AltSignalGateway delivers final `TradeSignal` payloads over an authenticated
TLS WebSocket. The binary protocol is protobuf; the reference client is
`altsignalgateway client` and its implementation is in
`src/altsignalgateway/cli/client.cpp`.

## Before connecting

- Obtain the endpoint, trusted certificate chain, `client_id`, `key_id` and
  secret from the service operator.
- Generate client bindings from `proto/gateway.proto` or the descriptor set
  shipped with the release.
- Advertise support for payload schema version 3 during `HELLO`.
- Keep the secret in protected secret storage. Verify the TLS certificate and
  hostname; do not disable certificate verification.

See [SIGNAL_CONTRACT.md](SIGNAL_CONTRACT.md) for payload fields and
[SECURITY.md](SECURITY.md) for authentication and authorization.

## Connection lifecycle

```text
TLS → WebSocket → HELLO → HELLO_OK → AUTH → AUTH_OK → RESUME
    → replayed SignalMessage frames → ACKs → RESUME_COMPLETE → READY
    → live SignalMessage frames ↔ ACKs and heartbeats
```

Frames are binary protobuf `ClientFrame` and `ServerFrame` messages. The
protocol uses a challenge-response HMAC over TLS. Each connection receives a
new session identifier and nonce. See [WEBSOCKET_PROTOCOL.md](WEBSOCKET_PROTOCOL.md)
for frame definitions, state transitions, timeouts and limits.

## Capabilities

Client credentials explicitly grant capabilities:

- `receive` permits signal delivery;
- `ack` permits acknowledgements that advance that client's durable cursor;
- `resume` permits a client to select a replay position;
- `query` permits protocol-2 read-only signal, history and context queries.

Use a client identity with `ack` only when that consumer owns the durable
delivery cursor. An observer without `ack` can read the stream without moving
that cursor.

## Resume and acknowledgement

Resume mode is explicit:

| mode | effect |
|---|---|
| `RESUME_FROM_SEQUENCE` | Continue from the greater of the supplied sequence and the server's durable cursor. |
| `RESUME_FROM_START` | Replay the retained, unexpired journal from the beginning. |
| `RESUME_LIVE_ONLY` | Begin at the current journal head for this session. |

An ACK confirms protocol-level receipt; it does not confirm order placement or
execution. Use `signal_id` as the downstream idempotency key because delivery is
at least once. Acknowledge only after the signal is durably handled according to
the consumer's own processing policy.

Signals past `expires_at_ms` are not delivered. Their skipped sequence numbers
are reported in resume metadata. Respect the signal validity and entry-fill
deadlines in the payload.

## Signal integrity

The client receives a canonical payload and its SHA-256 hash. Recompute the
hash over the specified canonical bytes before processing the signal. The
payload also carries the original upstream envelope for lineage verification.
Journalled payloads are immutable; a corrected proposal arrives under a new
signal identity.

## Read-only queries

Protocol version 2 adds queries for a journalled signal, a bounded history page
and point-in-time context. Queries require the `query` capability, use the
limits in `query` configuration and return errors in-band. They do not change
the delivery cursor or journal state.

## Reference client

```sh
altsignalgateway client --help
```

The reference client supports receipt, resume, acknowledgement and protocol-2
queries. It verifies hashes and is useful for validating a consumer
implementation against the service contract.
