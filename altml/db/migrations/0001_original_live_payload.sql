-- Forward-compatible extension; leave db/schema.sql and its historical digest
-- unchanged. Existing binaries ignore this nullable column. No backfill or
-- reconstruction of old inputs: NULL means original live bytes are unavailable.
ALTER TABLE altml.feature_vectors ADD COLUMN IF NOT EXISTS original_live_payload BYTEA;
COMMENT ON COLUMN altml.feature_vectors.original_live_payload IS
    'First original live AltTech envelope bytes; independent of a preexisting backfill vector';
