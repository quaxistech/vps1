# Delivery contract

AltSignalGateway provides **at-least-once** delivery. A network failure can
occur after the client received a message but before its acknowledgement
reaches the server, so the server may deliver the same signal again. Consumers
must process messages idempotently using `signal_id`.

## Journal and acknowledgement

Each accepted input is assigned a persistent signal identity and journal
sequence. Journal identity and payload are immutable. Delivery state changes as
the service offers a signal, receives an ACK, expires the signal or marks it
permanently undeliverable.

```text
JOURNALED → QUEUED → SENT → ACKED
     │          │       ├→ JOURNALED  (session ends before ACK)
     └──────────┴───────┴→ EXPIRED    (validity window ends)
```

`FAILED` records a payload that cannot be delivered under the integrity
contract. `ACKED`, `EXPIRED` and `FAILED` are terminal journal states.

An ACK means that the client received the message at the protocol level. It
does not confirm order placement, execution or position state. It advances only
the cursor for the authenticated client.

## Client cursor

`client_cursors.last_acked_sequence` is a contiguous watermark: every sequence
at or below it has been acknowledged by that client, expired before delivery or
otherwise permanently not owed. A later ACK cannot move the cursor past an
earlier message that is still outstanding.

For each session, the server tracks outstanding offered sequences and the
highest sequence examined by the delivery pump. The watermark is the sequence
before the earliest outstanding message, or the highest examined sequence when
none remain outstanding. The database rejects backwards cursor movement.

`RESUME_LIVE_ONLY` begins at the current journal head for that connection and
does not persist a cursor update. `RESUME_FROM_SEQUENCE` resumes using the
greater of the client-provided position and its durable server cursor.

## Multiple clients

Each `client_id` has its own cursor. `signal_journal.delivery_state` is an
aggregate state for the signal; a signal acknowledged by one client can still
be owed to another client whose cursor has not advanced past it. Per-client
session activity is recorded in `delivery_events`.

Concurrent sessions configured for one client identity share that identity's
cursor. Consumers that process concurrently must coordinate acknowledgements
for their own identity.

## Ingest idempotency

| received message | result |
|---|---|
| Same `signal_id` and same upstream bytes | Keep the existing journal row and sequence. |
| Same `signal_id` with different upstream bytes | Keep the original row and record an integrity violation. |

The source payload hash is the idempotency comparison. The canonical payload
may gain enrichment fields across service versions; the original producer bytes
remain the immutable identity evidence.

## Expiry

Signals are not offered after `expires_at_ms`. The optional
`ingest.max_signal_age_sec` adds an earlier age bound measured from signal T0.
Expired sequences may be skipped during replay; resume metadata reports the
count skipped. See [WEBSOCKET_PROTOCOL.md](WEBSOCKET_PROTOCOL.md) for resume
frame details.
