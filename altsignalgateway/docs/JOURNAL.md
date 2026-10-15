# The journal

The journal is the delivery contract. A signal committed here has been promised
to the external consumer; a signal not here was never promised. Everything else
— the WebSocket queue, the session state, the in-flight batch — is a cache of
this table, rebuilt from it after any restart.

Schema: `db/migrations/0001_altsignalgateway_schema.sql`. In production it is
a schema of the `altstrategy` database, next to the `altstrategy` schema whose
context snapshots enrich every signal; AltSpotter's instrument registry and
AltTech's catalogue are read through the `altspotter_ref` / `alttech_ref`
foreign schemas (`postgres_fdw`, owned by `altsignalgateway_app`).

## `signal_journal`

One row per FINAL TradeSignal. Immutable in everything that describes the trade;
mutable only in what describes the delivery of it.

| column | type | meaning |
|---|---|---|
| `signal_id` | text **PK** | AltSignalQuality's `final_signal_id`. One trade idea can never occupy two rows, however many times it is redelivered. |
| `message_id` | text **unique** | `asgw_` + 32 hex, deterministic in `signal_id` |
| `sequence` | bigint **unique** | gapless, strictly increasing, allocated in the same transaction |
| `signal_proposal_id`, `candidate_id`, `evaluation_id` | text | lineage upstream |
| `canonical_pair_id`, `exchange`, `symbol`, `side`, `strategy_id`, `strategy_version` | | denormalised so an operator can answer "what did we send for XRPUSDT this morning" without decoding a protobuf |
| `schema_version`, `protocol_version` | integer | |
| `created_at_ms` | bigint | **T0** |
| `expires_at_ms` | bigint | `proposal.valid_until_ms` |
| `received_at_ms`, `journaled_at_ms` | bigint | arrival, and durable commit |
| `source_payload` | bytea | **the exact bytes AltSignalQuality put on NATS** |
| `source_payload_sha256` | text | |
| `source_subject`, `source_stream`, `source_stream_sequence` | | where it came from |
| `canonical_payload` | bytea | the deterministic serialization of `SignalPayload` — the byte string that is hashed, transmitted and re-transmitted |
| `payload_sha256` | text | |
| `delivery_state` | text | JOURNALED \| QUEUED \| SENT \| ACKED \| EXPIRED \| FAILED |
| `delivery_attempts` | integer | monotone |
| `first_sent_at_ms`, `last_delivery_at_ms`, `last_ack_at_ms` | bigint | |
| `acked_by_client_id`, `last_session_id` | text | |
| `failure_reason` | text | required when `FAILED` |
| `warnings` | text[] | non-fatal contract observations from ingest |

### Why both payloads are stored

`canonical_payload` embeds `source_payload` (as `SignalPayload.source_envelope`),
so storing the source separately is a deliberate duplication. It buys two
things:

* the source column can be read, hashed and compared **without parsing**
  protobuf — which is what makes `verify` a meaningful independent check rather
  than the encoder checking itself;
* the idempotency arbiter is the **source** hash, not the canonical one. The
  canonical projection can legitimately change between builds (a new enrichment
  field, a registry that has since resolved a symbol) while the upstream bytes
  are the trade itself. Comparing the projection would report an integrity
  violation on every upgrade.

## Invariants, enforced by the database

Not by convention, and not only by the process — a second process, an operator
with `psql` or a future refactor cannot produce an impossible history.

| invariant | mechanism |
|---|---|
| one row per signal | `signal_id` primary key |
| unique message id | unique constraint |
| unique, positive sequence | unique constraint + `chk_journal_sequence_positive` |
| a valid window | `chk_journal_validity` (`expires_at_ms > created_at_ms`) |
| hashes are hashes | `chk_journal_hash_shape` (`^[0-9a-f]{64}$`) |
| **identity and payload are immutable** | `trg_asgw_journal_immutable` — an `UPDATE` touching `signal_id`, `sequence`, either payload, either hash, `created_at_ms`, `expires_at_ms`, `journaled_at_ms` or `schema_version` raises |
| **only legal delivery transitions** | `trg_asgw_delivery_transition`, mirroring `domain/delivery_state.hpp` |
| `delivery_attempts` never falls | same trigger — a falling count would let a redelivery loop hide itself |
| `ACKED` rows carry evidence | `chk_journal_acked_evidence` — an ack time *and* a client id |
| `SENT`/`ACKED` rows carry evidence | `chk_journal_sent_evidence` — a first-sent time |
| `FAILED` rows carry a reason | `chk_journal_failed_reason` |
| rows are deleted only by retention | `trg_asgw_journal_delete_guard` — any other `DELETE` raises |
| a client cursor never rewinds | `trg_asgw_cursor_monotonic` |

## The sequence

```sql
CREATE TABLE altsignalgateway.sequence_state (
    singleton     BOOLEAN PRIMARY KEY DEFAULT true CHECK (singleton),
    last_sequence BIGINT NOT NULL DEFAULT 0,
    updated_at    TIMESTAMPTZ NOT NULL DEFAULT now()
);
```

Allocation, inside the journalling transaction:

```sql
UPDATE altsignalgateway.sequence_state
   SET last_sequence = last_sequence + 1, updated_at = now()
 WHERE singleton RETURNING last_sequence;
```

The row lock is held to commit, so allocation is strictly serialized:

* **gapless** — a rolled-back transaction releases the number,
* **monotone**,
* **visible in allocation order** — a lower sequence can never become visible
  after a higher one,
* **restart-safe** — it is a durable row, not a counter and not a scan of the
  journal for a maximum.

A `BIGSERIAL` gives none of the first three. `RESUME` is defined as "everything
above the last acknowledged sequence", so a gap is a message the client will
never ask for again and an out-of-order commit is one it has already skipped
past. `tests/shell/test_schema_invariants.sh` asserts no `BIGSERIAL` appears in
the migration.

## Supporting tables

| table | contents | retention |
|---|---|---|
| `client_cursors` | `client_id`, `last_acked_sequence`, protocol version, client version, last seen, total acked. **The whole of what this service remembers about a client** — no credentials, no keys, no addresses beyond the last observed one. | none (one row per client) |
| `delivery_events` | one row per QUEUED / SENT / REPLAYED / ACKED / REQUEUED / EXPIRED / FAILED, with client, session and attempt. | 180 d |
| `sessions` | one row per authenticated session: peer, protocol version, resume mode and position, counts, close reason. A `key_id`, never a key. | 180 d |
| `rejected_messages` | inbound messages that do not satisfy the signal contract, with their raw bytes | 90 d |
| `integrity_violations` | repeated `signal_id` values carrying different source payloads and hashes | 1000 d |
| `retention_policies` / `retention_runs` | the policy table and its audit | — |
| `service_state` | one row: instance, start time, counters, last signal | — |

## Retention

Per-table and time-based, driven by `retention_policies`. The pass runs every
`retention_interval_sec` (6 h), sets a session GUC the `DELETE` guard checks,
and records what it removed in `retention_runs`.

| table | days | rationale |
|---|---|---|
| `signal_journal` | **400** | persistent record of signals offered for external delivery |
| `delivery_events` | 180 | how each signal reached the consumer |
| `sessions` | 180 | connection audit |
| `rejected_messages` | 90 | operational, not a trading record |
| `integrity_violations` | 1000 | long-term identity and payload integrity record |

Nothing needed for audit, replay, model lineage, investigation or recovery is
removed inside those windows. The journal window is deliberately the longest on
the stack.

## Storage

Journal storage is bounded by the `signal_journal` retention policy. Payloads
are stored as protobuf bytes; PostgreSQL indexes, delivery events and client
sessions use additional space. `altsignalgateway status` reports journal bytes
and row counts. Capacity planning should use observed message sizes and the
configured retention window for the deployment.

## Reading the journal

```sh
altsignalgateway status                          # counts, head, backlog, clients, sessions
altsignalgateway inspect --sequence 101          # the canonical payload as JSON
altsignalgateway inspect --sequence 101 --source # the original upstream envelope as JSON
altsignalgateway verify --limit 5000             # re-hash and re-serialize; report any mismatch
```

`inspect` recomputes the payload hash and reports it alongside the stored value.

`verify` performs four independent checks per row — the canonical hash, the
source hash, that the embedded original matches the source column and its
provenance hash, and that re-serializing the parsed payload reproduces the
stored bytes exactly.

The last one is a *schema-agreement* check, not an integrity one: the delivery
path does not re-serialize journalled payloads; it hashes and sends the stored
bytes. Canonical serialization checks confirm that the running schema preserves
the payload representation.
