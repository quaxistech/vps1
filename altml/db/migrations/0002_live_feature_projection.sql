-- Preserve the first-wins historical vector, and separately retain the exact
-- input accepted from the live subscription. A previous backfill may contain
-- a different recomputation at the same bar revision. Historical receipts are
-- not reconstructed; NULL is explicit absence until a real live observation.
ALTER TABLE altml.feature_vectors
    ADD COLUMN IF NOT EXISTS original_live_values REAL[],
    ADD COLUMN IF NOT EXISTS original_live_validity_bitmap BYTEA,
    ADD COLUMN IF NOT EXISTS original_live_computed_at TIMESTAMPTZ,
    ADD COLUMN IF NOT EXISTS original_live_received_at TIMESTAMPTZ,
    ADD COLUMN IF NOT EXISTS original_live_engine_version TEXT;

-- NOT VALID: enforced for every row written from now on, without scanning the
-- archive (tens of GB) under an exclusive lock. Rows that predate this
-- migration hold NULL in every column added above, so they satisfy the check.
DO $migration$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM pg_constraint
                   WHERE conrelid = 'altml.feature_vectors'::regclass
                     AND conname = 'feature_vectors_live_projection_complete') THEN
        ALTER TABLE altml.feature_vectors ADD CONSTRAINT feature_vectors_live_projection_complete CHECK (
            (original_live_values IS NULL AND original_live_validity_bitmap IS NULL
             AND original_live_computed_at IS NULL AND original_live_received_at IS NULL
             AND original_live_engine_version IS NULL)
            OR
            (original_live_payload IS NOT NULL AND original_live_values IS NOT NULL
             AND original_live_validity_bitmap IS NOT NULL
             AND original_live_computed_at IS NOT NULL AND original_live_received_at IS NOT NULL
             AND original_live_engine_version IS NOT NULL)) NOT VALID;
    END IF;
END
$migration$;

-- One projection for econ training, replay and restart priming. Original
-- historical fields never change. Late live receipt remains late even when
-- its source computation predates a previously archived target.
CREATE OR REPLACE VIEW altml.econ_feature_vectors AS
SELECT exchange_id, instrument_id, timeframe, bar_open_time, bar_revision,
       feature_set_name, feature_set_version, schema_hash,
       COALESCE(original_live_engine_version, engine_version) AS engine_version,
       COALESCE(original_live_values, values) AS values,
       COALESCE(original_live_validity_bitmap, validity_bitmap) AS validity_bitmap,
       quality_status,
       COALESCE(original_live_computed_at, computed_at) AS computed_at,
       COALESCE(original_live_received_at, received_at) AS received_at,
       CASE WHEN original_live_received_at IS NOT NULL THEN 'live' ELSE ingest_path END AS ingest_path
FROM altml.feature_vectors;

-- The upgrade may be applied by an administrator. Serving remains owned by
-- the same role as the underlying AltML archive rather than the deploy role.
DO $owner$
DECLARE archive_owner TEXT;
BEGIN
    SELECT pg_get_userbyid(relowner) INTO archive_owner FROM pg_class
    WHERE oid = 'altml.feature_vectors'::regclass;
    EXECUTE format('ALTER VIEW altml.econ_feature_vectors OWNER TO %I', archive_owner);
END
$owner$;
