-- The macro/cross-asset block.
--
-- A new scope, because these features belong to no venue and no pair. USDCNH
-- does not depend on which crypto instrument is being predicted, so storing one
-- row per pair would duplicate the same numbers across every instrument and
-- turn a 400k-row block into an 11M-row one for no information at all.
--
-- Everything else follows the shape the pipeline already uses: an ordered value
-- array, a validity bitmap saying which positions may be read, and a status.

BEGIN;

ALTER TABLE alttech.feature_set_versions DROP CONSTRAINT IF EXISTS feature_set_versions_scope_check;
ALTER TABLE alttech.feature_set_versions ADD CONSTRAINT feature_set_versions_scope_check
    CHECK (scope IN ('VENUE','CROSS_VENUE','BITGET_PROJECTED','GLOBAL'));

CREATE TABLE IF NOT EXISTS alttech.macro_feature_vectors (
    timeframe              TEXT        NOT NULL
        CHECK (timeframe IN ('1m','5m','15m','30m','1h','4h','1d')),
    bar_open_time          TIMESTAMPTZ NOT NULL,
    feature_set_version_id BIGINT      NOT NULL REFERENCES alttech.feature_set_versions(id),
    method_version         TEXT        NOT NULL,
    values                 REAL[]      NOT NULL,
    -- Bit i set means values[i] may be read. A clear bit means the inputs were
    -- missing, not yet published, or too stale -- and the zero sitting in the
    -- array is a placeholder, never a measurement.
    validity_bitmap        BYTEA       NOT NULL,
    status                 TEXT        NOT NULL DEFAULT 'VALID'
        CHECK (status IN ('NOT_READY','WARMING','VALID','INCOMPLETE_INPUT','REVISED','INVALID')),
    -- How many positions are readable. Lets a consumer filter on completeness
    -- without unpacking the bitmap.
    valid_count            INTEGER     NOT NULL DEFAULT 0,
    computed_at            TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at             TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (timeframe, bar_open_time, feature_set_version_id)
) PARTITION BY RANGE (bar_open_time);

CREATE INDEX IF NOT EXISTS idx_macro_fv_time
    ON alttech.macro_feature_vectors (bar_open_time);

COMMIT;

BEGIN;

-- The hourly partition sweep walks this list. A table missing from it gets no
-- partitions, and every write then fails with "no partition found for row" --
-- which is exactly how this stack lost a night once already.
CREATE OR REPLACE FUNCTION alttech.partitioned_parents()
RETURNS TEXT[]
LANGUAGE sql
IMMUTABLE
AS $$
    SELECT ARRAY[
        'alttech.venue_feature_vectors_bitget',
        'alttech.venue_feature_vectors_reference',
        'alttech.venue_feature_projection',
        'alttech.venue_microstructure_vectors_bitget_sub',
        'alttech.venue_microstructure_vectors_bitget_1m',
        'alttech.venue_microstructure_vectors_ref_1m',
        'alttech.structure_snapshots',
        'alttech.structure_zone_events',
        'alttech.venue_regimes',
        'alttech.global_regimes',
        'alttech.regime_events',
        'alttech.cross_venue_feature_vectors',
        'alttech.cross_venue_quality',
        'alttech.venue_alignment_state',
        'alttech.cross_venue_components',
        'alttech.cross_venue_snapshots',
        'alttech.macro_feature_vectors'];
$$;

COMMIT;

BEGIN;

-- The scope vocabulary lives in two places: the set version and the individual
-- definition. Widening only the first let the service start and then fail on
-- its first definition insert -- loudly, which is right, but the fix belongs
-- with the rest of the change.
ALTER TABLE alttech.feature_definitions DROP CONSTRAINT IF EXISTS feature_definitions_scope_check;
ALTER TABLE alttech.feature_definitions ADD CONSTRAINT feature_definitions_scope_check
    CHECK (scope IN ('VENUE','CROSS_VENUE','BITGET_PROJECTED','GLOBAL'));

COMMIT;
