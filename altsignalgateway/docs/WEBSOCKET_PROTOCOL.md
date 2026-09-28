# The WebSocket protocol

```
protocol_version 2   (1 still served: a protocol-1 client gets the protocol-1 contract)
transport        TLS 1.3 (1.2 configurable) → WebSocket → protobuf, BINARY frames
endpoint         wss://<gateway-host>:9443/
```

Protocol 2 adds one client frame, `QueryRequest`, and one server frame,
`QueryResponse`: one journalled signal by identity, a bounded keyset page of
the history, or the extended market context of a signal. They need the `query`
capability, are accepted from `AUTHENTICATED` onwards, read only, and answer
errors in-band (`ERR_INVALID_QUERY`, `ERR_NOT_FOUND`, `ERR_QUERY_LIMIT`)
without closing the connection. The [consumer guide](CONSUMER_GUIDE.md)
summarizes client integration; wire message definitions are in
`proto/gateway.proto`.

Every frame is a serialized `altsignalgateway.v1.ClientFrame` or `ServerFrame`
in a **binary** WebSocket frame. A text frame is a protocol error: the protocol
is protobuf, and a text frame is either a different protocol or a probe.

The reference implementation of the client half is
`src/altsignalgateway/cli/client.cpp`, reachable as
`altsignalgateway client`. It shares the canonical auth string and the frame
codecs with the server, so the two cannot drift apart.

## Frames

| direction | frame | purpose |
|---|---|---|
| C→S | `Hello` | announce protocol version, identity and supported schema versions |
| S→C | `HelloOk` | issue the session id and the single-use server nonce |
| C→S | `Auth` | prove the identity with an HMAC over the challenge |
| S→C | `AuthOk` | grant capabilities, report the durable cursor and journal head |
| S→C | `ProtocolError` | refuse, with a machine-readable code |
| C→S | `Resume` | state where to start: from a sequence, live only, or from the beginning |
| S→C | `ResumeOk` | confirm the start point and how much is owed |
| S→C | `SignalMessage` | one FINAL TradeSignal (`replay=true` during the backlog) |
| C→S | `Ack` | confirm protocol-level receipt of one message |
| S→C | `ResumeComplete` | the backlog is drained |
| S→C | `Ready` | the live stream begins |
| S→C | `Heartbeat` / C→S | `HeartbeatAck` | liveness, with a nonce |
| either | `Goodbye` | orderly close |

## Lifecycle

```
   client                                   server
     │  ── TCP connect ─────────────────────► │
     │  ◄─ TLS 1.3 handshake ──────────────► │   server cert verified by the client
     │  ── WebSocket upgrade ──────────────► │
     │                                        │   [CONNECTED]
     │  ── HELLO ──────────────────────────► │   protocol_version, client_id,
     │      client_nonce (≥16 B)              │   supported_schema_versions
     │  ◄─ HELLO_OK ─────────────────────── │   session_id, server_nonce (32 B CSPRNG),
     │                                        │   auth_scheme, server_time, skew budget
     │                                        │   [HELLO_SENT]
     │  ── AUTH ───────────────────────────► │   key_id, timestamp, echoed nonce,
     │      HMAC-SHA256 signature             │   32-byte signature
     │  ◄─ AUTH_OK ──────────────────────── │   capabilities, heartbeat budget,
     │                                        │   last_acked_sequence, journal_head
     │                                        │   [AUTHENTICATED]
     │  ── RESUME(mode, last_acked) ───────► │
     │  ◄─ RESUME_OK ───────────────────── │   from_sequence_exclusive, backlog_count,
     │                                        │   skipped_expired, journal_head
     │                                        │   [RESUMING]
     │  ◄─ SIGNAL (replay=true) ×N ──────── │
     │  ── ACK ────────────────────────────► │   (may be sent during the replay)
     │  ◄─ RESUME_COMPLETE ─────────────── │
     │  ◄─ READY ────────────────────────── │   [READY]
     │  ◄─ SIGNAL (replay=false) ────────── │   live
     │  ── ACK ────────────────────────────► │
     │  ◄─ HEARTBEAT ────────────────────── │   every 15 s
     │  ── HEARTBEAT_ACK ─────────────────► │
```

After a disconnect the client reconnects and repeats the whole sequence. There
is no session resumption at the transport level: a new connection means a new
session id and a new nonce, which is precisely what makes a captured `Auth`
frame worthless.

## State machine

```
CONNECTED ──HELLO──► HELLO_SENT ──AUTH ok──► AUTHENTICATED ──RESUME──► RESUMING
                          │                        │                       │
                     AUTH fail                 (timeout)          (backlog drained)
                          ▼                        ▼                       ▼
                       CLOSING ◄───────────────────┴──────────────────  READY
                          │
                          ▼
                       CLOSED
```

What each state accepts — exhaustively, and asserted by
`tests/unit/test_protocol.cpp`:

| state | accepts |
|---|---|
| CONNECTED | `Hello`, `Goodbye` |
| HELLO_SENT | `Auth`, `Goodbye` |
| AUTHENTICATED | `Resume`, `HeartbeatAck`, `QueryRequest`†, `Goodbye` |
| RESUMING | `Ack`, `HeartbeatAck`, `QueryRequest`†, `Goodbye` |
| READY | `Ack`, `HeartbeatAck`, `QueryRequest`†, `Goodbye` |
| CLOSING | `Goodbye` |
| CLOSED | nothing |

† protocol 2, with the `query` capability; one in flight per session.

Anything else is `ERR_PROTOCOL_VIOLATION` and closes the connection. In
particular a **second `Hello` or `Auth` on a live connection is refused**:
re-authenticating would let a captured `Auth` frame be replayed against a
session whose nonce has already been consumed.

`Ack` is accepted during `RESUMING` as well as `READY` — requiring otherwise
would force a consumer to buffer an entire backlog before it could confirm any
of it.

## Authentication — ASGW-AUTH-v1

HMAC-SHA256 challenge-response over TLS. See [SECURITY.md](SECURITY.md) for why
this scheme and not a bearer token or mTLS alone.

The string both sides sign, newline-separated with every component in fixed
order:

```
ASGW-AUTH-v1\n
<protocol_version>\n
<client_id>\n
<key_id>\n
<session_id>\n
<hex(server_nonce)>\n
<hex(client_nonce)>\n
<timestamp_ms>
```

`signature = HMAC-SHA256(secret, canonical_string)` — 32 raw bytes.

The server verifies, in order: the `key_id` is known; the key's owner matches
the `client_id` claimed in `HELLO`; the echoed nonce is byte-identical to the
one **this connection** issued; `timestamp_ms` is within `max_clock_skew_sec`;
the signature matches in constant time; the credential has the `receive`
capability.

A failure returns `ERR_AUTH_FAILED` with no further detail. The operator's log
records which check failed; telling a caller *why* its signature was rejected is
a free oracle for refining an attack.

## Authorization

`AuthOk` lists exactly what the credential may do:

| capability | grants |
|---|---|
| `CAP_RECEIVE_SIGNALS` | receive `SignalMessage` frames |
| `CAP_ACK` | send `Ack` (which advances **this client's** durable cursor) |
| `CAP_RESUME` | send `Resume` |
| `CAP_QUERY` | send `QueryRequest` (protocol 2; never advertised to a protocol-1 session) |

There is no fifth capability, because there is no frame in the protocol that
does anything else. A client cannot reach PostgreSQL, cannot reach NATS, cannot
alter a signal and cannot write anything into VPS1 — the only state it can move
is its own cursor. A read-only observer is configured by omitting `ack`.

## RESUME

`Resume.mode` must be stated explicitly. `RESUME_MODE_UNSPECIFIED` is rejected:
"start where I left off" and "send me everything" differ by an entire journal,
and a resume intent is never guessed.

| mode | start point |
|---|---|
| `RESUME_FROM_SEQUENCE` | `max(client's last_acked_sequence, server's durable cursor)` |
| `RESUME_LIVE_ONLY` | the journal head — nothing historical. **This session writes no cursor:** skipping the backlog is a decision about one connection, not a durable abandonment, so a later `RESUME_FROM_SEQUENCE` still receives everything the client was owed. |
| `RESUME_FROM_START` | sequence 0 — the whole retained, non-expired journal |

`ResumeOk` reports `from_sequence_exclusive`, the measured `backlog_count` and
`skipped_expired` in that range, and the journal head. `Ready.journal_head_
sequence` carries the journal's real head, read fresh at that moment — not the
position the replay happened to stop at, which differs whenever the tail of the
replay window had expired.

**Safety.** Each of the following is a real case and each has a defined answer:

| the client says | the server does |
|---|---|
| a sequence ahead of the journal head | `ERR_INVALID_SEQUENCE`, closes. Nothing destructive: the client keeps whatever it has. |
| a negative sequence | `ERR_INVALID_SEQUENCE`, closes. |
| a sequence **below** its own durable cursor | uses the server's cursor. Re-sending acknowledged trades on a client's say-so is how a duplicate order gets placed; the cursor is durable precisely so delivery does not depend on the client's memory. |
| a sequence above its cursor but within the journal | honours it — a client may legitimately have acknowledged more than the server recorded. |
| `RESUME` a second time on one connection | `ERR_PROTOCOL_VIOLATION` — the state machine does not accept it. |
| `RESUME` without the `resume` capability | `ERR_NOT_AUTHORIZED`. |
| no `RESUME` within `resume_timeout_sec` | `ERR_TIMEOUT`, closes. |
| two connections resuming at once | each gets its own session and its own scan cursor; the shared durable cursor only ever moves forward (`GREATEST`, plus a database trigger). |

Signals whose `expires_at_ms` has passed are **skipped**, never delivered. The
sequence the client sees therefore has gaps, and `skipped_expired` is why.

## ACK

```
Ack { message_id, signal_id, sequence, client_time_ms }
```

An ACK means: **the client received and accepted this message at the protocol
level.** It does **not** mean the trade was executed, an order was accepted, or
a position was opened. Execution feedback is a different contract on a different
path and is not part of this protocol.

The ACK must match the message the server sent. A mismatch in `signal_id` or
`sequence` is `ERR_INVALID_SEQUENCE` and closes the connection: the two sides'
bookkeeping disagree, and advancing a cursor on a mismatch would skip a trade.

An ACK for a message the server is not holding (a late or duplicate one after a
reconnect) is **logged and ignored**, not a violation — it is legal client
behaviour under at-least-once delivery.

## Heartbeat

The server sends `Heartbeat` with a fresh nonce every
`heartbeat_interval_sec` (default 15) from `AUTHENTICATED` onwards. The client
echoes the nonce in `HeartbeatAck`. Silence for `heartbeat_timeout_sec`
(default 45) closes the connection with `ERR_HEARTBEAT_TIMEOUT`.

Application-level, not WebSocket ping/pong: a ping/pong is answered by the
transport and proves only that the TCP stack is alive. This heartbeat proves the
*application* is reading its socket. The nonce proves the answer belongs to the
beat that was sent rather than to an older one a stalled client is still working
through.

Beast's own timeout covers the TCP/TLS handshake phase, where the application
heartbeat does not exist yet, and is disarmed afterwards — an idle session with
no signals to receive is the normal state of this endpoint and must not be
killed for it.

## Limits and backpressure

| limit | default | on breach |
|---|---|---|
| `max_message_bytes` | 1 MiB | refused by the transport **before buffering**; `ERR_MESSAGE_TOO_LARGE`, close |
| `max_queued_messages` | 256 | `ERR_QUEUE_OVERFLOW`, close |
| `max_queued_bytes` | 16 MiB | `ERR_QUEUE_OVERFLOW`, close |
| `ack_timeout_sec` | 120 | `ERR_TIMEOUT`, close |
| `max_clients` | 8 | `ERR_TOO_MANY_CLIENTS`, close |
| `handshake_timeout_sec` | 10 | `ERR_TIMEOUT`, close |
| `resume_timeout_sec` | 30 | `ERR_TIMEOUT`, close |

There is deliberately no separate frame-size setting. A fragmented message is
reassembled and refused at the same bound, so a second knob would have measured
the same quantity twice and read like a protection that does not exist. Control
frames are bounded by the WebSocket protocol itself (125-byte payloads).

The same bound applies **inbound at ingest**: a FINAL signal whose canonical
payload would not fit inside it is refused as `PAYLOAD_TOO_LARGE` and never
journalled. Journalling it would promise a delivery this endpoint could not make
— a conforming consumer would refuse the frame and disconnect, and the signal
would be redelivered forever.

The delivery pump never produces past the queue bound — it asks each session how
much it can take before reading the journal — so in normal operation the queue
limits are unreachable. They exist for the case where the socket stops draining
and control frames accumulate behind it, which is what a client whose TCP window
has closed looks like: healthy at the protocol level, right up until the process
runs out of memory.

A slow client is therefore disconnected, not buffered. Everything it was holding
returns to the journal and it collects it through `RESUME` on the next connect.

A final `ProtocolError` bypasses the queue bound: refusing to tell a client why
it is being disconnected *because its queue is full* would defeat the purpose of
the limit.

## Duplicate sessions

**Policy: newest wins.** When an authenticated `client_id` exceeds its
configured `max_sessions`, the **oldest** session for that client is closed with
`ERR_SESSION_SUPERSEDED`.

Rejected alternative: refusing the new connection. After a network partition the
server frequently still holds a half-dead socket that will not fail until its
heartbeat budget runs out, and locking a reconnecting consumer out of its own
feed for that long is the worse failure. Nothing is lost either way — the
superseded session requeues everything it held and the new one collects it.

Applied under the session registry's lock, so two simultaneous connections
cannot both decide they are the newest.

## Reconnect behaviour

The client initiates every reconnect; the server never dials out. A flapping
client costs one session slot per live connection and nothing else: an
unauthenticated connection already occupies a slot (which is what bounds the
cheapest denial of service), the handshake timeout reclaims it, and repeated
authentication failures from one peer trigger a per-peer lock-out
(`auth_failure_threshold` within `auth_failure_window_sec` ⇒ `auth_lockout_sec`).
A successful authentication clears the counter, so a legitimate client that
simply restarted is never locked out.

## Error codes

| code | meaning | fatal |
|---|---|---|
| `ERR_PROTOCOL_VIOLATION` | a frame illegal in the current state | yes |
| `ERR_UNSUPPORTED_PROTOCOL_VERSION` | | yes |
| `ERR_UNSUPPORTED_SCHEMA_VERSION` | the client cannot read what this server emits | yes |
| `ERR_AUTH_FAILED` | authentication failed, deliberately unspecific | yes |
| `ERR_NOT_AUTHORIZED` | authenticated, but lacks the capability | yes |
| `ERR_MESSAGE_TOO_LARGE` | | yes |
| `ERR_MALFORMED_FRAME` | not a valid `ClientFrame`, or not binary | yes |
| `ERR_INVALID_SEQUENCE` | a resume position or an ACK that cannot be honoured | yes |
| `ERR_RESUME_REJECTED` | resume mode unstated or unsupported | yes |
| `ERR_QUEUE_OVERFLOW` | slow consumer | yes |
| `ERR_HEARTBEAT_TIMEOUT` | | yes |
| `ERR_SESSION_SUPERSEDED` | a newer session for this client authenticated | yes |
| `ERR_TOO_MANY_CLIENTS` | | yes |
| `ERR_SERVER_SHUTTING_DOWN` | | yes |
| `ERR_TIMEOUT` | a handshake step or an ACK was not completed in time | yes |
| `ERR_INTERNAL` | | yes |
| `ERR_INVALID_QUERY` | a query filter, cursor, page size or identity the server cannot honour | no (in `QueryResponse`) |
| `ERR_NOT_FOUND` | no journalled signal with that identity | no (in `QueryResponse`) |
| `ERR_QUERY_LIMIT` | query rate, in-flight, queue or response bound exceeded | no (in `QueryResponse`) |

## A minimal client

```sh
altsignalgateway client \
    --host <gateway-host> --port 9443 --sni altsignalgateway \
    --ca-file /etc/altsignalgateway/tls/server.crt \
    --client-id <client-id> --key-id <key-id> \
    --secret-file /etc/altsignalgateway/clients/<client-id>.key \
    --resume from-sequence --last-acked 0
```

The secret is never a command-line argument: an argv secret is visible in `ps`
to every user on the box.
