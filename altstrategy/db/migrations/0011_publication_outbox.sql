-- Publication after archive (BUG-01 / BUG-02). Additive.
--
-- A proposal used to be published by a fire-and-forget core-NATS publish after
-- four separate database writes whose results were never read, so a signal
-- could reach AltSignalQuality with no archive behind it, and a publish the
-- stream never stored left no trace. Now the transaction that archives a
-- decision (candidate, plan variants, proposals) also writes the messages it
-- owes the bus here, and the publisher sends them to JetStream with
-- `message_id` as the Nats-Msg-Id, marking a row only once the stream has
-- acknowledged it. Nothing is published that this table did not receive in the
-- same commit as the record it describes.
--
-- A row is SETTLED once the stream has acknowledged it (published_at_ms,
-- stream_sequence). A row the publisher gave up on after its maximum age is
-- DEAD-LETTERED: kept with its payload and last error, counted in the
-- publisher's report, and sent again only when an operator requeues it
-- (db/operator/outbox_requeue_dead_lettered.sql). Retention keys on
-- settled_at, which is NULL for every row the stream has not acknowledged, so
-- the purge can delete neither a message still owed nor one given up on.
CREATE TABLE IF NOT EXISTS altstrategy.publication_outbox (
    message_id           TEXT PRIMARY KEY,
    kind                 TEXT NOT NULL
                             CHECK (kind IN ('signal_proposed', 'trade_proposal', 'market_outcome')),
    subject              TEXT NOT NULL,
    payload              BYTEA NOT NULL,
    signal_proposal_id   TEXT,
    candidate_id         TEXT,
    created_at_ms        BIGINT NOT NULL,
    attempts             INTEGER NOT NULL DEFAULT 0,
    next_attempt_at_ms   BIGINT NOT NULL,
    last_error           TEXT,
    published_at_ms      BIGINT,
    stream_sequence      BIGINT,
    dead_lettered_at_ms  BIGINT,
    settled_at           TIMESTAMPTZ,
    created_at           TIMESTAMPTZ NOT NULL DEFAULT now(),
    CONSTRAINT publication_outbox_one_settlement
        CHECK (published_at_ms IS NULL OR dead_lettered_at_ms IS NULL),
    CONSTRAINT publication_outbox_settled_iff_acknowledged
        CHECK ((settled_at IS NULL) = (published_at_ms IS NULL))
);

-- The publisher's scan: rows still owed, by due time.
CREATE INDEX IF NOT EXISTS publication_outbox_due
    ON altstrategy.publication_outbox (next_attempt_at_ms)
    WHERE settled_at IS NULL AND dead_lettered_at_ms IS NULL;
-- What was given up on: the report's count and the operator's requeue.
CREATE INDEX IF NOT EXISTS publication_outbox_dead_lettered
    ON altstrategy.publication_outbox (dead_lettered_at_ms) WHERE dead_lettered_at_ms IS NOT NULL;
-- Lineage: from a proposal to what was (or was not) published for it.
CREATE INDEX IF NOT EXISTS publication_outbox_proposal
    ON altstrategy.publication_outbox (signal_proposal_id);

INSERT INTO altstrategy.retention_policies (table_name, retain_days, time_column, is_critical)
VALUES ('publication_outbox', 14, 'settled_at', false)
ON CONFLICT (table_name) DO NOTHING;
