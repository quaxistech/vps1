-- Forward-only addition. Apply with alttech-schema --upgrade FILE; the
-- previously installed base schema checksum remains immutable.
CREATE TABLE alttech.analytics_outbox (
    message_id TEXT PRIMARY KEY,
    subject TEXT NOT NULL,
    payload BYTEA NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp()
);
CREATE INDEX analytics_outbox_pending ON alttech.analytics_outbox (created_at, message_id);
