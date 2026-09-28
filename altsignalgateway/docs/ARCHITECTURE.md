# Architecture

## Where this service sits

```
AltSpotter → AltTech → AltML → AltStrategy → AltSignalQuality → AltSignalGateway → wss://
   market      analytics  forecast  decision     signal validation    BOUNDARY
```

AltSignalGateway is the external delivery boundary for the pipeline. It is
**not** another intelligent layer: it validates, enriches, journals and
transmits final signals produced upstream.

## What it is

Five things, and nothing else:

```
FINAL SIGNAL BOUNDARY     the contract validator for downstream consumers
DURABLE JOURNAL           the record of every final signal committed for delivery
SECURE WEBSOCKET SERVER   TLS 1.3, HMAC challenge-response, one protocol
DELIVERY PROTOCOL         at-least-once with explicit ACK and RESUME
INTEGRITY / PROVENANCE    a hash over every payload and the full lineage
```

## What it may never do

It never changes the trade. Not the entry, the stop, the target, the side, the
strategy, the quality score or the model output. There is no code path in this
service that writes a price.

It adds transport metadata it owns outright — a sequence, a message id,
timestamps and hashes — plus read-only point-in-time enrichment: market and
macro context, the venue-facing symbol and venue tick/lot constraints.

## The lifecycle of one signal

```
   AltSignalQuality publishes on signalquality.final.<token>.v1
                              │
                              ▼
   ┌───────────── ingest thread ─────────────┐
   │  fetch (durable pull consumer)           │
   │  parse altsignalquality.v1.Envelope      │
   │  validate the contract  ───► rejected_messages, term
   │  enrich  (market, macro, venue context)   │   ← read-only point-in-time reads
   │  canonicalize  →  SignalPayload          │
   │  sha256(canonical), sha256(source)       │
   │  refuse if it will not fit the wire      │
   │  ┌── ONE TRANSACTION ───────────────┐    │
   │  │ SELECT existing by signal_id     │    │  ← idempotency
   │  │ UPDATE sequence_state +1          │    │  ← row-locked allocation
   │  │ INSERT signal_journal             │    │
   │  └──────────────────────────────────┘    │
   │  ACK JetStream        ← only now          │
   └──────────────────────────────────────────┘
                              │ wake
                              ▼
   ┌──────────── delivery pump (one thread) ──┐
   │  drain queued journal writes (FIFO)      │
   │  for each READY/RESUMING session:        │
   │     credit = what the client can take    │
   │     SELECT … WHERE sequence > cursor      │
   │              LIMIT min(credit, batch)     │
   │     skip expired, verify the hash         │
   │     build ServerFrame, mark QUEUED        │
   │     hand the batch to the session         │
   └──────────────────────────────────────────┘
                              │ post to strand
                              ▼
   ┌──────────── session (Asio strand) ───────┐
   │  async_write  →  mark SENT (after write) │
   │  read ACK     →  mark ACKED, advance cursor
   │  heartbeat, timeouts, backpressure       │
   └──────────────────────────────────────────┘
```

## Threading

| thread | count | owns | touches PostgreSQL |
|---|---|---|---|
| supervisor | 1 | lifecycle, watchdog, status | status reads only |
| ingest | 1 | the NATS consumer, the journal write | yes |
| delivery pump | 1 | every delivery-side journal write, frame building | yes |
| I/O | 2 (configurable) | TLS, WebSocket frames, timers | **never** |

Two properties fall out of the single-threaded pump that would otherwise need a
lock spanning a socket:

* **Ordering.** Every durable transition a session causes — SENT, then ACKED —
  is queued in submission order and applied in that order, so the delivery state
  machine can never see an acknowledgement for a message it has not recorded as
  sent.
* **Isolation.** No database call ever runs on an I/O thread. A slow database
  slows delivery; it does not stall the heartbeats or the handshake of every
  other connection.

Sessions submit `JournalOp` values to the pump and never touch the store. Reads
a session needs during the handshake (its durable cursor, the backlog above a
sequence) are queued the same way and answered back onto the session's strand.

## Bounded memory

The journal *is* the queue. The pump asks each session how much it can take,
then reads at most that many rows. A session holds at most
`websocket.max_queued_messages` signals in RAM regardless of how far behind the
consumer is: a week-long backlog and an empty one cost the same memory.

## Enrichment reads

| block | source | selection |
|---|---|---|
| market snapshot | `altstrategy.decision_context_snapshots` | Context reference and revision from the proposal at T0 |
| venue symbol and execution constraints | AltSpotter instrument registries | `canonical_pair_id`, configured execution venue and the spec in force at T0 |
| macro snapshot | configured AltTech macro schema | Macro vector available for the proposal's T0 |

The context and instrument registries are read-only inputs. The AltTech schema
is configured through `alttech_schema` and may be a PostgreSQL foreign-data
wrapper schema. Unavailable context is represented as absent with a reason;
the service does not fabricate or substitute values.

See [SIGNAL_CONTRACT.md](SIGNAL_CONTRACT.md) for the field-by-field rules and
[JOURNAL.md](JOURNAL.md) for what is stored.

## Ownership

| resource | owner | this service |
|---|---|---|
| `ALTSIGNALQUALITY_SIGNALS` (JetStream) | AltSignalQuality | validates its shape, never creates or reconfigures it |
| the durable `signalgateway-final` | **this service** | creates and binds it |
| `altsignalgateway` schema | **this service** | full ownership |
| `altstrategy.decision_context_snapshots` | AltStrategy | `SELECT` only |
| `public.*` instrument registry | AltSpotter | `SELECT` only |
| the WebSocket endpoint | **this service** | full ownership |

It publishes on no NATS subject at all. Its output is the WebSocket.

## Single writer

`bootstrap()` takes a PostgreSQL session-level advisory lock and refuses to
start if another instance holds it. Two Gateway processes against one journal
would share a sequence while keeping two independent session registries and two
client-cursor caches, and the resulting delivery history would be impossible to
reason about. It is refused at startup rather than diagnosed afterwards.

## Independence from the consumer

Ingestion does not require an active WebSocket session. With no client
connected:

```
FINAL signal → validate → journal          works, unchanged
                        → delivery queue   nothing to deliver, nothing waits
```

The ingest loop never blocks on delivery, the pump idles when the registry is
empty, and the journal keeps accumulating. When a client eventually connects it
resumes from its cursor and receives everything still within its validity
window. This is asserted end to end by
[TESTING.md](TESTING.md)'s no-consumer case.
