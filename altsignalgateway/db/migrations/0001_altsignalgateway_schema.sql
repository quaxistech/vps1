-- AltSignalGateway schema — the durable journal behind VPS1's external boundary.
--
-- PLACEMENT. This schema lives INSIDE the shared AltSpotter database, next to
-- `altstrategy` and `altsignalquality`, for the same reason they do: every
-- enrichment read this service makes joins its own rows to AltStrategy's
-- context snapshots and AltSpotter's instrument registry. A separate database
-- would put a postgres_fdw hop on the hot path of every single signal, and this
-- stack has already measured what that costs.
--
-- OWNERSHIP. AltSignalGateway owns exactly the `altsignalgateway` schema. It
-- never creates, alters or drops anything in `altstrategy`, `altsignalquality`
-- or `public`; it reads those read-only via plain SELECT grants on the handful
-- of tables listed in scripts/db-bootstrap.sh.
--
-- WHAT THIS SCHEMA IS. The journal is the delivery contract. A signal that is
-- committed here has been promised to the external consumer; a signal that is
-- not here was never promised. Everything else — the WebSocket queue, the
-- session state, the in-flight batch — is a cache of this table and is rebuilt
-- from it after any restart.
--
-- Conventions (shared with altstrategy / altsignalquality):
--   * every timestamp column is TIMESTAMPTZ in UTC; wire times stay BIGINT ms;
--   * every id is deterministic in its natural key, so a NATS redelivery or a
--     restart reproduces the same row instead of a duplicate;
--   * retention is per-table and time-based, driven by retention_policies.

CREATE SCHEMA IF NOT EXISTS altsignalgateway;

-- =========================================================================
-- The persistent sequence.
--
-- A BIGSERIAL was deliberately NOT used. A sequence is exempt from transaction
-- rollback, so a failed insert burns a number and leaves a hole; worse, two
-- concurrent inserts can commit out of order, making a lower sequence become
-- visible AFTER a higher one. RESUME is defined as "everything above the last
-- acknowledged sequence", so a hole is a message the client will never ask for
-- again and an out-of-order commit is a message it has already skipped past.
--
-- A single-row counter updated inside the journalling transaction takes a row
-- lock that is held to commit. Allocation is therefore strictly serialized: the
-- sequence is gapless, strictly increasing, and becomes visible in allocation
-- order. That is what makes the cursor arithmetic in docs/DELIVERY.md sound.
-- =========================================================================

CREATE TABLE IF NOT EXISTS altsignalgateway.sequence_state (
    singleton     BOOLEAN PRIMARY KEY DEFAULT true CHECK (singleton),
    last_sequence BIGINT NOT NULL DEFAULT 0 CHECK (last_sequence >= 0),
    updated_at    TIMESTAMPTZ NOT NULL DEFAULT now()
);
INSERT INTO altsignalgateway.sequence_state (singleton) VALUES (true)
ON CONFLICT (singleton) DO NOTHING;

-- =========================================================================
-- The journal — one row per FINAL TradeSignal that VPS1 has promised to
-- deliver. Immutable in everything that describes the trade; mutable only in
-- what describes the delivery of it.
-- =========================================================================

CREATE TABLE IF NOT EXISTS altsignalgateway.signal_journal (
    -- Identity. signal_id is AltSignalQuality's final_signal_id, which is itself
    -- deterministic in AltStrategy's signal_proposal_id: one trade idea can
    -- never occupy two rows here, however many times it is redelivered.
    signal_id            TEXT PRIMARY KEY,
    message_id           TEXT NOT NULL UNIQUE,      -- asgw_<32 hex>, derived from signal_id
    sequence             BIGINT NOT NULL UNIQUE,
    signal_proposal_id   TEXT NOT NULL,
    candidate_id         TEXT NOT NULL DEFAULT '',
    evaluation_id        TEXT NOT NULL DEFAULT '',

    -- Denormalised routing/diagnostic columns. They exist so an operator can
    -- answer "what did we send for XRPUSDT this morning" without decoding a
    -- protobuf, and they are copies of what the payload already says.
    canonical_pair_id    BIGINT NOT NULL,
    exchange             TEXT NOT NULL DEFAULT '',
    symbol               TEXT NOT NULL DEFAULT '',
    side                 TEXT NOT NULL CHECK (side IN ('long','short')),
    strategy_id          TEXT NOT NULL DEFAULT '',
    strategy_version     TEXT NOT NULL DEFAULT '',

    schema_version       INTEGER NOT NULL,
    protocol_version     INTEGER NOT NULL,

    -- Point-in-time anchors, all UTC milliseconds.
    created_at_ms        BIGINT NOT NULL,           -- T0, the information cut-off
    expires_at_ms        BIGINT NOT NULL,           -- proposal.valid_until_ms
    received_at_ms       BIGINT NOT NULL,           -- arrival on the NATS consumer
    journaled_at_ms      BIGINT NOT NULL,           -- this row's durable commit

    -- The ORIGINAL authoritative payload: the exact bytes AltSignalQuality put
    -- on NATS. Kept forever (within retention), not just until publication:
    -- reconstructing a trade from a projection is how an execution layer ends
    -- up acting on an approximation, and this stack has already decided once
    -- that it will not do that.
    source_payload       BYTEA NOT NULL,
    source_payload_sha256 TEXT NOT NULL,
    source_subject       TEXT NOT NULL DEFAULT '',
    source_stream        TEXT NOT NULL DEFAULT '',
    source_stream_sequence BIGINT NOT NULL DEFAULT 0,

    -- The CANONICAL payload: the deterministic serialization of
    -- altsignalgateway.v1.SignalPayload. This is the byte string that is hashed,
    -- transmitted and re-transmitted. It embeds source_payload, so the two are
    -- provably the same signal rather than two independent descriptions of one.
    canonical_payload    BYTEA NOT NULL,
    payload_sha256       TEXT NOT NULL,

    -- Delivery. The only mutable part of this row.
    delivery_state       TEXT NOT NULL DEFAULT 'JOURNALED'
        CHECK (delivery_state IN ('JOURNALED','QUEUED','SENT','ACKED','EXPIRED','FAILED')),
    delivery_attempts    INTEGER NOT NULL DEFAULT 0 CHECK (delivery_attempts >= 0),
    first_sent_at_ms     BIGINT,
    last_delivery_at_ms  BIGINT,
    last_ack_at_ms       BIGINT,
    acked_by_client_id   TEXT,
    last_session_id      TEXT,
    failure_reason       TEXT,

    -- Non-fatal contract observations recorded at ingest (see
    -- domain/validation.cpp). They never block delivery; they make a drift in
    -- an upstream contract visible before it becomes a support case.
    warnings             TEXT[] NOT NULL DEFAULT '{}',

    created_at           TIMESTAMPTZ NOT NULL DEFAULT now(),

    CONSTRAINT chk_journal_sequence_positive CHECK (sequence > 0),
    CONSTRAINT chk_journal_validity CHECK (expires_at_ms > created_at_ms),
    CONSTRAINT chk_journal_hash_shape CHECK (
        payload_sha256 ~ '^[0-9a-f]{64}$' AND source_payload_sha256 ~ '^[0-9a-f]{64}$'),
    -- An ACKED row must record who acknowledged it and when. Without this a bug
    -- could mark a signal delivered with no evidence that anything received it.
    CONSTRAINT chk_journal_acked_evidence CHECK (
        delivery_state <> 'ACKED' OR (last_ack_at_ms IS NOT NULL AND acked_by_client_id IS NOT NULL)),
    CONSTRAINT chk_journal_sent_evidence CHECK (
        delivery_state NOT IN ('SENT','ACKED') OR first_sent_at_ms IS NOT NULL),
    CONSTRAINT chk_journal_failed_reason CHECK (
        delivery_state <> 'FAILED' OR failure_reason IS NOT NULL)
);

-- The delivery scan: "the next N deliverable rows above this cursor". A partial
-- index keeps it proportional to the backlog rather than to the archive.
CREATE INDEX IF NOT EXISTS idx_asgw_journal_deliverable
    ON altsignalgateway.signal_journal (sequence)
    WHERE delivery_state IN ('JOURNALED','QUEUED','SENT');
CREATE INDEX IF NOT EXISTS idx_asgw_journal_expiry
    ON altsignalgateway.signal_journal (expires_at_ms)
    WHERE delivery_state IN ('JOURNALED','QUEUED','SENT');
CREATE INDEX IF NOT EXISTS idx_asgw_journal_created
    ON altsignalgateway.signal_journal (created_at);
CREATE INDEX IF NOT EXISTS idx_asgw_journal_pair_time
    ON altsignalgateway.signal_journal (canonical_pair_id, created_at_ms DESC);
CREATE INDEX IF NOT EXISTS idx_asgw_journal_proposal
    ON altsignalgateway.signal_journal (signal_proposal_id);

-- -------------------------------------------------------------------------
-- Immutability.
--
-- Once committed, a journalled signal is a historical fact. The trigger below
-- refuses any UPDATE that changes what the signal IS, while allowing what the
-- delivery of it is. It is enforced in the database and not only in the process
-- because the guarantee has to survive a second process, an operator with psql
-- and a future refactor of the C++ that forgets the rule.
--
-- Correcting a signal is not an UPDATE. A corrected trade is a NEW signal with
-- its own identity, published upstream, and it arrives here as a new row.
-- -------------------------------------------------------------------------

CREATE OR REPLACE FUNCTION altsignalgateway.enforce_journal_immutability()
RETURNS TRIGGER AS $$
BEGIN
    IF NEW.signal_id IS DISTINCT FROM OLD.signal_id
       OR NEW.message_id IS DISTINCT FROM OLD.message_id
       OR NEW.sequence IS DISTINCT FROM OLD.sequence
       OR NEW.signal_proposal_id IS DISTINCT FROM OLD.signal_proposal_id
       OR NEW.canonical_pair_id IS DISTINCT FROM OLD.canonical_pair_id
       OR NEW.side IS DISTINCT FROM OLD.side
       OR NEW.symbol IS DISTINCT FROM OLD.symbol
       OR NEW.exchange IS DISTINCT FROM OLD.exchange
       OR NEW.schema_version IS DISTINCT FROM OLD.schema_version
       OR NEW.created_at_ms IS DISTINCT FROM OLD.created_at_ms
       OR NEW.expires_at_ms IS DISTINCT FROM OLD.expires_at_ms
       OR NEW.journaled_at_ms IS DISTINCT FROM OLD.journaled_at_ms
       OR NEW.source_payload IS DISTINCT FROM OLD.source_payload
       OR NEW.source_payload_sha256 IS DISTINCT FROM OLD.source_payload_sha256
       OR NEW.canonical_payload IS DISTINCT FROM OLD.canonical_payload
       OR NEW.payload_sha256 IS DISTINCT FROM OLD.payload_sha256
    THEN
        RAISE EXCEPTION
            'altsignalgateway.signal_journal is immutable in signal identity and payload '
            '(signal_id=%): a corrected trade is a new signal, never an edited one',
            OLD.signal_id
            USING ERRCODE = 'integrity_constraint_violation';
    END IF;
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trg_asgw_journal_immutable ON altsignalgateway.signal_journal;
CREATE TRIGGER trg_asgw_journal_immutable
    BEFORE UPDATE ON altsignalgateway.signal_journal
    FOR EACH ROW EXECUTE FUNCTION altsignalgateway.enforce_journal_immutability();

-- Deleting a journal row outside the retention path would erase the evidence
-- that VPS1 promised a trade. Retention sets a marker in a temporary GUC; any
-- other DELETE is refused.
CREATE OR REPLACE FUNCTION altsignalgateway.guard_journal_delete()
RETURNS TRIGGER AS $$
BEGIN
    IF current_setting('altsignalgateway.retention_pass', true) IS DISTINCT FROM 'on' THEN
        RAISE EXCEPTION
            'altsignalgateway.signal_journal rows are removed only by the retention pass '
            '(signal_id=%)', OLD.signal_id
            USING ERRCODE = 'integrity_constraint_violation';
    END IF;
    RETURN OLD;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trg_asgw_journal_delete_guard ON altsignalgateway.signal_journal;
CREATE TRIGGER trg_asgw_journal_delete_guard
    BEFORE DELETE ON altsignalgateway.signal_journal
    FOR EACH ROW EXECUTE FUNCTION altsignalgateway.guard_journal_delete();

-- -------------------------------------------------------------------------
-- The delivery state machine, enforced in the database.
--
-- The same table lives in domain/delivery_state.hpp. Both exist on purpose:
-- the C++ copy stops the process from attempting an illegal write, and this
-- copy makes an illegal history impossible regardless of what wrote it.
--
--   JOURNALED -> QUEUED | EXPIRED | FAILED
--   QUEUED    -> SENT | JOURNALED | EXPIRED | FAILED
--   SENT      -> ACKED | JOURNALED | EXPIRED | FAILED
--   ACKED / EXPIRED / FAILED are terminal.
--
-- ACKED -> ACKED is permitted and is a no-op: at-least-once delivery means a
-- client can legitimately acknowledge the same message twice.
-- -------------------------------------------------------------------------

CREATE OR REPLACE FUNCTION altsignalgateway.enforce_delivery_transition()
RETURNS TRIGGER AS $$
DECLARE
    ok BOOLEAN := false;
BEGIN
    IF NEW.delivery_state = OLD.delivery_state THEN
        ok := (NEW.delivery_state = 'ACKED') OR (NEW.delivery_state IN ('JOURNALED','QUEUED','SENT'));
    ELSE
        ok := CASE OLD.delivery_state
            WHEN 'JOURNALED' THEN NEW.delivery_state IN ('QUEUED','EXPIRED','FAILED')
            WHEN 'QUEUED'    THEN NEW.delivery_state IN ('SENT','JOURNALED','EXPIRED','FAILED')
            WHEN 'SENT'      THEN NEW.delivery_state IN ('ACKED','JOURNALED','EXPIRED','FAILED')
            ELSE false
        END;
    END IF;

    IF NOT ok THEN
        RAISE EXCEPTION 'illegal delivery transition % -> % (signal_id=%)',
            OLD.delivery_state, NEW.delivery_state, OLD.signal_id
            USING ERRCODE = 'integrity_constraint_violation';
    END IF;

    -- delivery_attempts is a monotone count of transmissions. Letting it fall
    -- would let a redelivery loop hide itself.
    IF NEW.delivery_attempts < OLD.delivery_attempts THEN
        RAISE EXCEPTION 'delivery_attempts may not decrease (signal_id=%)', OLD.signal_id
            USING ERRCODE = 'integrity_constraint_violation';
    END IF;
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trg_asgw_delivery_transition ON altsignalgateway.signal_journal;
CREATE TRIGGER trg_asgw_delivery_transition
    BEFORE UPDATE OF delivery_state, delivery_attempts ON altsignalgateway.signal_journal
    FOR EACH ROW EXECUTE FUNCTION altsignalgateway.enforce_delivery_transition();

-- =========================================================================
-- Per-client delivery cursor.
--
-- The ONLY state the Gateway keeps about a client, and deliberately the
-- smallest set that makes RESUME work: an identity, a durable position, the
-- protocol it speaks and when it was last seen. No credentials, no session
-- keys, no addresses beyond the last one observed for diagnostics.
-- =========================================================================

CREATE TABLE IF NOT EXISTS altsignalgateway.client_cursors (
    client_id            TEXT PRIMARY KEY,
    last_acked_sequence  BIGINT NOT NULL DEFAULT 0 CHECK (last_acked_sequence >= 0),
    protocol_version     INTEGER NOT NULL DEFAULT 0,
    client_version       TEXT NOT NULL DEFAULT '',
    last_seen_at_ms      BIGINT NOT NULL DEFAULT 0,
    last_ack_at_ms       BIGINT NOT NULL DEFAULT 0,
    last_session_id      TEXT NOT NULL DEFAULT '',
    total_acked          BIGINT NOT NULL DEFAULT 0,
    updated_at           TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- A cursor that moves backwards would silently re-deliver acknowledged trades.
-- It is a bug wherever it comes from, so it is refused here rather than logged.
CREATE OR REPLACE FUNCTION altsignalgateway.enforce_cursor_monotonic()
RETURNS TRIGGER AS $$
BEGIN
    IF NEW.last_acked_sequence < OLD.last_acked_sequence THEN
        RAISE EXCEPTION
            'client cursor may not move backwards (client_id=%, % -> %)',
            OLD.client_id, OLD.last_acked_sequence, NEW.last_acked_sequence
            USING ERRCODE = 'integrity_constraint_violation';
    END IF;
    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trg_asgw_cursor_monotonic ON altsignalgateway.client_cursors;
CREATE TRIGGER trg_asgw_cursor_monotonic
    BEFORE UPDATE ON altsignalgateway.client_cursors
    FOR EACH ROW EXECUTE FUNCTION altsignalgateway.enforce_cursor_monotonic();

-- =========================================================================
-- Delivery audit.
--
-- One row per transmission and per acknowledgement. The journal says where a
-- signal IS; this table says how it got there. It is what a delivery dispute
-- is settled with, and it is sized so that a signal delivered a hundred times
-- is visible as such rather than being one row with a counter.
-- =========================================================================

CREATE TABLE IF NOT EXISTS altsignalgateway.delivery_events (
    id           BIGINT PRIMARY KEY GENERATED BY DEFAULT AS IDENTITY,
    signal_id    TEXT NOT NULL,
    sequence     BIGINT NOT NULL,
    client_id    TEXT NOT NULL DEFAULT '',
    session_id   TEXT NOT NULL DEFAULT '',
    event        TEXT NOT NULL CHECK (event IN
                     ('QUEUED','SENT','ACKED','REQUEUED','EXPIRED','FAILED','REPLAYED')),
    attempt      INTEGER NOT NULL DEFAULT 0,
    replay       BOOLEAN NOT NULL DEFAULT false,
    detail       TEXT NOT NULL DEFAULT '',
    occurred_at_ms BIGINT NOT NULL,
    created_at   TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS idx_asgw_delivery_events_signal
    ON altsignalgateway.delivery_events (signal_id, id);
CREATE INDEX IF NOT EXISTS idx_asgw_delivery_events_created
    ON altsignalgateway.delivery_events (created_at);

-- =========================================================================
-- Session audit — one row per authenticated WebSocket session.
--
-- Deliberately without any credential material: a key id, never a key.
-- =========================================================================

CREATE TABLE IF NOT EXISTS altsignalgateway.sessions (
    session_id       TEXT PRIMARY KEY,
    client_id        TEXT NOT NULL,
    key_id           TEXT NOT NULL DEFAULT '',
    peer_address     TEXT NOT NULL DEFAULT '',
    protocol_version INTEGER NOT NULL DEFAULT 0,
    client_version   TEXT NOT NULL DEFAULT '',
    resume_mode      TEXT NOT NULL DEFAULT '',
    resume_from      BIGINT NOT NULL DEFAULT 0,
    replayed_count   BIGINT NOT NULL DEFAULT 0,
    delivered_count  BIGINT NOT NULL DEFAULT 0,
    acked_count      BIGINT NOT NULL DEFAULT 0,
    opened_at_ms     BIGINT NOT NULL,
    closed_at_ms     BIGINT,
    close_reason     TEXT NOT NULL DEFAULT '',
    created_at       TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS idx_asgw_sessions_client
    ON altsignalgateway.sessions (client_id, opened_at_ms DESC);
CREATE INDEX IF NOT EXISTS idx_asgw_sessions_created
    ON altsignalgateway.sessions (created_at);

-- =========================================================================
-- Rejected inbound messages — contract violations at the boundary.
--
-- A message that fails validation is NOT journalled: the journal is a promise
-- of delivery, and an unexecutable message is not something to promise. It is
-- recorded here instead, with its raw bytes, so an upstream contract break is
-- diagnosable from the database rather than from a log that has rotated away.
-- =========================================================================

CREATE TABLE IF NOT EXISTS altsignalgateway.rejected_messages (
    id              BIGINT PRIMARY KEY GENERATED BY DEFAULT AS IDENTITY,
    signal_id       TEXT NOT NULL DEFAULT '',
    subject         TEXT NOT NULL DEFAULT '',
    stream_sequence BIGINT NOT NULL DEFAULT 0,
    rejection_code  TEXT NOT NULL,
    detail          TEXT NOT NULL DEFAULT '',
    payload         BYTEA,
    payload_sha256  TEXT NOT NULL DEFAULT '',
    payload_bytes   INTEGER NOT NULL DEFAULT 0,
    received_at_ms  BIGINT NOT NULL,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS idx_asgw_rejected_created
    ON altsignalgateway.rejected_messages (created_at);

-- =========================================================================
-- Integrity violations — the same signal id carrying different bytes.
--
-- This is the one inbound condition that must never be resolved by overwriting.
-- If AltSignalQuality ever republished final_signal_id X with a different
-- payload, one of the two is the trade that VPS1 actually decided on and the
-- Gateway cannot know which. It keeps the FIRST, refuses the second, and
-- records both hashes here so the discrepancy is investigable.
-- =========================================================================

CREATE TABLE IF NOT EXISTS altsignalgateway.integrity_violations (
    id                  BIGINT PRIMARY KEY GENERATED BY DEFAULT AS IDENTITY,
    signal_id           TEXT NOT NULL,
    existing_sha256     TEXT NOT NULL,
    incoming_sha256     TEXT NOT NULL,
    existing_sequence   BIGINT NOT NULL,
    subject             TEXT NOT NULL DEFAULT '',
    stream_sequence     BIGINT NOT NULL DEFAULT 0,
    incoming_payload    BYTEA,
    detected_at_ms      BIGINT NOT NULL,
    created_at          TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS idx_asgw_integrity_signal
    ON altsignalgateway.integrity_violations (signal_id);

-- =========================================================================
-- Retention. Sized per table for its own purpose.
--
-- The journal's window is deliberately the longest on the stack: it is the only
-- record of what VPS1 told the outside world to do, and an execution dispute or
-- a model-lineage question can arrive months after the trade.
-- =========================================================================

CREATE TABLE IF NOT EXISTS altsignalgateway.retention_policies (
    table_name   TEXT PRIMARY KEY,
    retain_days  INTEGER NOT NULL CHECK (retain_days > 0),
    time_column  TEXT NOT NULL,
    is_critical  BOOLEAN NOT NULL DEFAULT false,
    rationale    TEXT NOT NULL DEFAULT '',
    updated_at   TIMESTAMPTZ NOT NULL DEFAULT now()
);

INSERT INTO altsignalgateway.retention_policies
    (table_name, retain_days, time_column, is_critical, rationale)
VALUES
    ('signal_journal', 400, 'created_at', true,
     'the only record of what VPS1 asked the outside world to execute; audit, replay and model lineage all read it'),
    ('delivery_events', 180, 'created_at', true,
     'how each signal reached the consumer; the evidence a delivery dispute is settled with'),
    ('sessions', 180, 'created_at', false,
     'connection audit; superseded by delivery_events for anything trade-related'),
    ('rejected_messages', 90, 'created_at', false,
     'upstream contract breaks; operational, not a trading record'),
    ('integrity_violations', 1000, 'created_at', true,
     'a signal id that carried two payloads is a stack-level defect and is kept far longer than the signals themselves')
ON CONFLICT (table_name) DO NOTHING;

CREATE TABLE IF NOT EXISTS altsignalgateway.retention_runs (
    id            BIGINT PRIMARY KEY GENERATED BY DEFAULT AS IDENTITY,
    started_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
    finished_at   TIMESTAMPTZ,
    rows_deleted  BIGINT NOT NULL DEFAULT 0,
    detail        JSONB NOT NULL DEFAULT '{}',
    error         TEXT
);

-- =========================================================================
-- Service state — a single row the status command and the operator read.
-- =========================================================================

CREATE TABLE IF NOT EXISTS altsignalgateway.service_state (
    singleton              BOOLEAN PRIMARY KEY DEFAULT true CHECK (singleton),
    instance_id            TEXT NOT NULL DEFAULT '',
    started_at_ms          BIGINT NOT NULL DEFAULT 0,
    last_signal_id         TEXT NOT NULL DEFAULT '',
    last_signal_at_ms      BIGINT NOT NULL DEFAULT 0,
    last_ack_at_ms         BIGINT NOT NULL DEFAULT 0,
    signals_journalled     BIGINT NOT NULL DEFAULT 0,
    signals_rejected       BIGINT NOT NULL DEFAULT 0,
    updated_at             TIMESTAMPTZ NOT NULL DEFAULT now()
);
INSERT INTO altsignalgateway.service_state (singleton) VALUES (true)
ON CONFLICT (singleton) DO NOTHING;
