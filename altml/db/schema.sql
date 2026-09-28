-- AltML database schema — dedicated `altml` schema in AltML's own database.
--
-- AltML owns exactly this schema and nothing else: it never creates, alters
-- or drops anything in AltSpotter's or AltTech's databases. Everything it
-- reads from those services is exposed read-only through postgres_fdw
-- foreign tables (created by scripts/db-bootstrap.sh, which also creates the
-- FOREIGN SERVER/USER MAPPING objects — those need real host/credentials and
-- must never be committed to this file).
--
-- The file is idempotent (CREATE ... IF NOT EXISTS) and applied by
-- scripts/db-bootstrap.sh, which records its checksum in altml.schema_version.
--
-- Conventions
--   * every timestamp is TIMESTAMPTZ in UTC;
--   * time-series tables are RANGE-partitioned by month so retention is
--     enforced by whole-partition DROP, never row-by-row DELETE;
--   * AltML holds no foreign keys onto another service's tables — external
--     identifiers (exchange_id, instrument_id, canonical_pair_id) are
--     referenced by value, read through the *_fdw foreign tables below;
--   * retention per table is sized for its own purpose, not one blanket
--     number: order lifetime is at most 48h, but model training needs a
--     rolling multi-month window of features+labels to walk-forward over.

CREATE SCHEMA IF NOT EXISTS altml;

CREATE TABLE IF NOT EXISTS altml.schema_version (
    singleton  BOOLEAN PRIMARY KEY DEFAULT true CHECK (singleton),
    checksum   TEXT NOT NULL,
    applied_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- ---------------------------------------------------------------------------
-- Partition helper. Namespaced to `altml` so AltML partition maintenance can
-- only ever touch its own tables.
-- ---------------------------------------------------------------------------

-- Idempotently create monthly range partitions of `parent` covering
-- [from_ts, to_ts]. Partition names are <parent>_YYYYMM.
CREATE OR REPLACE FUNCTION altml.ensure_month_partitions(
    parent  regclass,
    from_ts TIMESTAMPTZ,
    to_ts   TIMESTAMPTZ)
RETURNS INTEGER
LANGUAGE plpgsql
AS $$
DECLARE
    lo      TIMESTAMPTZ;
    hi      TIMESTAMPTZ;
    pname   TEXT;
    created INTEGER := 0;
BEGIN
    lo := date_trunc('month', from_ts AT TIME ZONE 'UTC') AT TIME ZONE 'UTC';
    IF lo > to_ts THEN
        RAISE EXCEPTION 'invalid partition range: % > %', from_ts, to_ts;
    END IF;
    WHILE lo <= to_ts LOOP
        hi := lo + interval '1 month';
        pname := parent::text || '_' || to_char(lo AT TIME ZONE 'UTC', 'YYYYMM');
        IF to_regclass(pname) IS NULL THEN
            EXECUTE format('CREATE TABLE %s PARTITION OF %s FOR VALUES FROM (%L) TO (%L)',
                           pname, parent::text, lo, hi);
            created := created + 1;
        END IF;
        lo := hi;
    END LOOP;
    RETURN created;
END;
$$;

-- Daily range partitions.
--
-- The feature archive needs day granularity, not month. Measured on the test
-- VPS with all four venues and all seven grids flowing, the archive grows about
-- 1.1 GB/day, so a MONTHLY partition reaches ~34 GB -- larger than the whole
-- storage budget. Retention's only lever is dropping a partition, so a month
-- sized partition means retention can free either nothing or everything. Days
-- give it a lever proportional to the decision it is actually making.
CREATE OR REPLACE FUNCTION altml.ensure_day_partitions(
    parent  regclass,
    from_ts TIMESTAMPTZ,
    to_ts   TIMESTAMPTZ)
RETURNS INTEGER
LANGUAGE plpgsql
AS $$
DECLARE
    lo      TIMESTAMPTZ;
    hi      TIMESTAMPTZ;
    pname   TEXT;
    created INTEGER := 0;
BEGIN
    lo := date_trunc('day', from_ts AT TIME ZONE 'UTC') AT TIME ZONE 'UTC';
    IF lo > to_ts THEN
        RAISE EXCEPTION 'invalid partition range: % > %', from_ts, to_ts;
    END IF;
    WHILE lo <= to_ts LOOP
        hi := lo + interval '1 day';
        pname := parent::text || '_' || to_char(lo AT TIME ZONE 'UTC', 'YYYYMMDD');
        IF to_regclass(pname) IS NULL THEN
            BEGIN
                EXECUTE format('CREATE TABLE %s PARTITION OF %s FOR VALUES FROM (%L) TO (%L)',
                               pname, parent::text, lo, hi);
                created := created + 1;
            EXCEPTION WHEN invalid_object_definition OR duplicate_table THEN
                -- A legacy monthly partition still covers this day. Skip it
                -- rather than fail: the cutover is deliberately gradual, and a
                -- day already covered needs no partition of its own.
                NULL;
            END;
        END IF;
        lo := hi;
    END LOOP;
    RETURN created;
END;
$$;

-- List the range partitions of `parent` with their bounds (retention drops).
CREATE OR REPLACE FUNCTION altml.partition_bounds(parent regclass)
RETURNS TABLE (partition_name TEXT, lower_bound TIMESTAMPTZ, upper_bound TIMESTAMPTZ)
LANGUAGE plpgsql
AS $$
DECLARE
    r    RECORD;
    expr TEXT;
    lo   TEXT;
    hi   TEXT;
BEGIN
    FOR r IN
        SELECT c.oid, n.nspname, c.relname
        FROM pg_inherits i
        JOIN pg_class c ON c.oid = i.inhrelid
        JOIN pg_namespace n ON n.oid = c.relnamespace
        WHERE i.inhparent = parent
    LOOP
        expr := pg_get_expr((SELECT relpartbound FROM pg_class WHERE oid = r.oid), r.oid);
        lo := substring(expr FROM 'FROM \(''([^'']+)''\)');
        hi := substring(expr FROM 'TO \(''([^'']+)''\)');
        IF lo IS NOT NULL AND hi IS NOT NULL THEN
            partition_name := r.nspname || '.' || r.relname;
            lower_bound := lo::timestamptz;
            upper_bound := hi::timestamptz;
            RETURN NEXT;
        END IF;
    END LOOP;
END;
$$;

-- ---------------------------------------------------------------------------
-- Fee model. Round-trip taker fees + a flagged slippage estimate, versioned
-- so a label set can freeze the exact economics it was computed with.
-- ---------------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS altml.fee_models (
    id                     BIGINT PRIMARY KEY GENERATED BY DEFAULT AS IDENTITY,
    version                INTEGER NOT NULL UNIQUE,
    maker_fee              NUMERIC NOT NULL,
    taker_fee              NUMERIC NOT NULL,
    slippage_estimate_bps  NUMERIC,  -- flagged estimate, not a measured value
    note                   TEXT NOT NULL DEFAULT '',
    created_at             TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- Round-trip taker fees for Bitget USDT-M (0.06% x 2); slippage marked as a
-- rough estimate until microstructure calibration replaces it.
INSERT INTO altml.fee_models (version, maker_fee, taker_fee, slippage_estimate_bps, note)
VALUES (1, 0.0002, 0.0006, 2.0, 'bitget usdt-m default; slippage=estimate')
ON CONFLICT (version) DO NOTHING;

-- ---------------------------------------------------------------------------
-- Label catalog. Labels describe FUTURE outcomes of a bar and are kept
-- strictly separate from features. One row = one label vector per 1m bar.
-- ---------------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS altml.label_definitions (
    id              BIGINT PRIMARY KEY GENERATED BY DEFAULT AS IDENTITY,
    name            TEXT NOT NULL UNIQUE,
    horizon_minutes INTEGER NOT NULL,
    kind            TEXT NOT NULL,  -- return|return_after_fees|mfe|mae|volatility|direction|barrier
    description     TEXT NOT NULL DEFAULT '',
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS altml.label_set_versions (
    id                BIGINT PRIMARY KEY GENERATED BY DEFAULT AS IDENTITY,
    name              TEXT NOT NULL,
    version           INTEGER NOT NULL,
    fee_model_version INTEGER NOT NULL REFERENCES altml.fee_models(version),
    builder_version   TEXT NOT NULL,
    schema_hash       TEXT NOT NULL,
    frozen            BOOLEAN NOT NULL DEFAULT false,
    created_at        TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (name, version)
);

CREATE TABLE IF NOT EXISTS altml.label_set_members (
    label_set_version_id BIGINT NOT NULL REFERENCES altml.label_set_versions(id),
    position             INTEGER NOT NULL,
    definition_id        BIGINT NOT NULL REFERENCES altml.label_definitions(id),
    output_name          TEXT NOT NULL,
    PRIMARY KEY (label_set_version_id, position),
    UNIQUE (label_set_version_id, output_name)
);

-- Future-outcome labels for closed 1m Bitget bars. Retention target: the
-- rolling training lookback (30 days, training.lookback_days) plus margin for
-- the weekly retrain cadence and a slow cycle — NOT the 48h order lifetime,
-- this is training substrate that must span many walk-forward folds.
CREATE TABLE IF NOT EXISTS altml.labels (
    exchange_id          SMALLINT    NOT NULL,
    instrument_id        BIGINT      NOT NULL,
    timeframe            TEXT        NOT NULL DEFAULT '1m',
    bar_open_time        TIMESTAMPTZ NOT NULL,
    bar_revision         BIGINT      NOT NULL DEFAULT 0,
    label_set_version_id BIGINT      NOT NULL,
    values               REAL[]      NOT NULL,
    validity_bitmap      BYTEA       NOT NULL,
    computed_at          TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (exchange_id, instrument_id, timeframe, bar_open_time,
                 bar_revision, label_set_version_id)
) PARTITION BY RANGE (bar_open_time);

CREATE TABLE IF NOT EXISTS altml.label_computation_runs (
    id            BIGINT PRIMARY KEY GENERATED BY DEFAULT AS IDENTITY,
    service       TEXT NOT NULL,
    exchange_id   SMALLINT NOT NULL,
    instrument_id BIGINT,
    range_from    TIMESTAMPTZ,
    range_to      TIMESTAMPTZ,
    rows_written  BIGINT NOT NULL DEFAULT 0,
    status        TEXT NOT NULL DEFAULT 'running'
        CHECK (status IN ('running','completed','failed','aborted')),
    error         TEXT,
    started_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
    finished_at   TIMESTAMPTZ
);

-- ---------------------------------------------------------------------------
-- Local feature-vector archive.
--
-- AltTech keeps its own `venue_feature_vectors` for only a few days (its
-- retention is sized for live order attribution, not model training). AltML
-- consumes the same live NATS feature stream for inference and archives
-- every VALID vector here as it arrives, building its own long-enough
-- training substrate organically from real, observed market data — nothing
-- here is backfilled or synthesized. Identified by (name, version,
-- schema_hash) rather than AltTech's internal numeric id: AltML holds no
-- foreign key onto another service's table.
-- ---------------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS altml.feature_vectors (
    exchange_id         SMALLINT    NOT NULL,
    instrument_id       BIGINT      NOT NULL,
    timeframe           TEXT        NOT NULL DEFAULT '1m',
    bar_open_time       TIMESTAMPTZ NOT NULL,
    bar_revision        BIGINT      NOT NULL DEFAULT 0,
    feature_set_name    TEXT        NOT NULL,
    feature_set_version INTEGER     NOT NULL,
    schema_hash         TEXT        NOT NULL,
    engine_version      TEXT        NOT NULL,
    values              REAL[]      NOT NULL,
    validity_bitmap     BYTEA       NOT NULL,
    quality_status      TEXT        NOT NULL DEFAULT 'VALID'
        CHECK (quality_status IN ('VALID','DEGRADED','INVALID')),
    computed_at         TIMESTAMPTZ NOT NULL,
    received_at         TIMESTAMPTZ NOT NULL DEFAULT now(),
    -- Which path put this row here.
    --
    -- The two writers do NOT always agree, and the difference is real: AltTech
    -- computes a live vector from a warm engine and later recomputes the same
    -- bar from a bounded replay, leaving ~640 of 995 positions different at the
    -- SAME bar_revision. Live wins by ON CONFLICT DO NOTHING, which is the
    -- parity-correct choice -- production inference consumes live vectors, so a
    -- model must be trained on live-regime features. But a dataset that mixes
    -- the two silently is a dataset nobody can reason about, so every row says
    -- which regime produced it.
    -- 'unknown' is the honest default for rows written before this column
    -- existed: stamping them 'live' would assert a provenance nobody verified.
    ingest_path         TEXT NOT NULL DEFAULT 'unknown'
                        CHECK (ingest_path IN ('live', 'backfill', 'unknown')),
    PRIMARY KEY (exchange_id, instrument_id, timeframe, bar_open_time,
                 bar_revision, schema_hash)
) PARTITION BY RANGE (bar_open_time);

CREATE INDEX IF NOT EXISTS idx_feature_vectors_lookup
    ON altml.feature_vectors (exchange_id, instrument_id, timeframe, bar_open_time DESC);

-- ---------------------------------------------------------------------------
-- Model registry. Training only records CANDIDATE models; promotion to
-- ACTIVE is the trainer's own audited transactional step.
-- ---------------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS altml.models (
    id                   BIGINT PRIMARY KEY GENERATED BY DEFAULT AS IDENTITY,
    model_id             TEXT NOT NULL UNIQUE,       -- e.g. 20260710T000000Z-lightgbm-a1b2c3
    model_version        INTEGER NOT NULL DEFAULT 1,
    algorithm            TEXT NOT NULL,              -- lightgbm
    objective            TEXT NOT NULL,               -- e.g. binary:y_direction_15m
    feature_set_version  TEXT NOT NULL,               -- talib-core:1
    label_set_version    TEXT NOT NULL,               -- core-labels:1
    dataset_id           TEXT NOT NULL,
    training_period      TSTZRANGE,
    validation_period    TSTZRANGE,
    test_period          TSTZRANGE,
    hyperparameters      JSONB NOT NULL DEFAULT '{}',
    status               TEXT NOT NULL DEFAULT 'TRAINING'
        CHECK (status IN ('TRAINING','FAILED','CANDIDATE','VALIDATED','APPROVED',
                          'ACTIVE','REJECTED','RETIRED','ROLLBACK')),
    created_at           TIMESTAMPTZ NOT NULL DEFAULT now(),
    approved_at          TIMESTAMPTZ,
    activated_at         TIMESTAMPTZ,
    retired_at           TIMESTAMPTZ
);

-- Exactly one ACTIVE model per objective at a time.
CREATE UNIQUE INDEX IF NOT EXISTS uq_altml_models_active_per_objective
    ON altml.models (objective) WHERE status = 'ACTIVE';

CREATE TABLE IF NOT EXISTS altml.model_artifacts (
    id          BIGINT PRIMARY KEY GENERATED BY DEFAULT AS IDENTITY,
    model_id    TEXT NOT NULL REFERENCES altml.models(model_id),
    kind        TEXT NOT NULL,        -- model | feature_names | sample_predictions | report
    path        TEXT NOT NULL,
    format      TEXT NOT NULL,        -- lightgbm-text | json
    checksum    TEXT NOT NULL,        -- sha256
    size_bytes  BIGINT NOT NULL,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (model_id, kind)
);

CREATE TABLE IF NOT EXISTS altml.model_metrics (
    id          BIGINT PRIMARY KEY GENERATED BY DEFAULT AS IDENTITY,
    model_id    TEXT NOT NULL REFERENCES altml.models(model_id),
    scope       TEXT NOT NULL,        -- validation | test | fold:<n>
    metric      TEXT NOT NULL,
    value       DOUBLE PRECISION NOT NULL,
    UNIQUE (model_id, scope, metric)
);

CREATE TABLE IF NOT EXISTS altml.model_walk_forward_runs (
    id          BIGINT PRIMARY KEY GENERATED BY DEFAULT AS IDENTITY,
    model_id    TEXT NOT NULL REFERENCES altml.models(model_id),
    fold        INTEGER NOT NULL,
    train_from  TIMESTAMPTZ NOT NULL,
    train_to    TIMESTAMPTZ NOT NULL,
    valid_from  TIMESTAMPTZ NOT NULL,
    valid_to    TIMESTAMPTZ NOT NULL,
    test_from   TIMESTAMPTZ,
    test_to     TIMESTAMPTZ,
    metrics     JSONB NOT NULL DEFAULT '{}',
    UNIQUE (model_id, fold)
);

-- What a model needs on its input, in enough detail to REBUILD that input.
--
-- `feature_count` is the width of the matrix the booster was trained on, which
-- is NOT the width of the vector AltTech publishes: the trainer appends
-- point-in-time observations from other venues and grids. `base_feature_count`
-- is the published vector's width, and the three aligned columns are the recipe
-- for the rest. Inference reconstructs the same columns in the same order from
-- them, so a model can only be served by an input built the way it was trained.
CREATE TABLE IF NOT EXISTS altml.model_feature_compatibility (
    model_id             TEXT NOT NULL REFERENCES altml.models(model_id),
    feature_set_version  TEXT NOT NULL,
    schema_hash          TEXT NOT NULL,
    feature_count        INTEGER NOT NULL,
    PRIMARY KEY (model_id, feature_set_version)
);
ALTER TABLE altml.model_feature_compatibility
    ADD COLUMN IF NOT EXISTS base_feature_count INTEGER NOT NULL DEFAULT 0,
    ADD COLUMN IF NOT EXISTS aligned_sources    TEXT[] NOT NULL DEFAULT '{}',
    ADD COLUMN IF NOT EXISTS aligned_projection TEXT[] NOT NULL DEFAULT '{}';

-- The input recipe of an econ model (see src/altml/components/feature_recipe.hpp)
-- and its calibration. A model is served only when every one of these can be
-- reproduced; an empty recipe_version marks a pre-recipe (legacy) model, which
-- the econ inference path refuses rather than guesses at.
ALTER TABLE altml.model_feature_compatibility
    ADD COLUMN IF NOT EXISTS recipe_version       TEXT   NOT NULL DEFAULT '',
    ADD COLUMN IF NOT EXISTS base_projection      TEXT[] NOT NULL DEFAULT '{}',
    ADD COLUMN IF NOT EXISTS feature_names_sha256 TEXT   NOT NULL DEFAULT '',
    ADD COLUMN IF NOT EXISTS calibration          JSONB  NOT NULL DEFAULT '{}'::jsonb;

-- Every live estimate an econ model made: which model, on which bar, what it
-- said and against which cost. What the decision layer then did with it lives
-- in AltStrategy's own records (candidate -> ml evaluation -> outcome), keyed
-- back to (model_id, instrument, bar). Monthly partitions, dropped whole.
CREATE TABLE IF NOT EXISTS altml.model_predictions (
    exchange_id            SMALLINT    NOT NULL,
    instrument_id          BIGINT      NOT NULL,
    bar_open_time          TIMESTAMPTZ NOT NULL,
    bar_revision           BIGINT      NOT NULL,
    model_id               TEXT        NOT NULL,
    objective              TEXT        NOT NULL,
    horizon_minutes        INTEGER     NOT NULL,
    p_short                REAL        NOT NULL,
    p_neutral              REAL        NOT NULL,
    p_long                 REAL        NOT NULL,
    expected_return        REAL        NOT NULL,   -- simple return over the horizon, gross
    expected_net_long_bps  REAL        NOT NULL,
    expected_net_short_bps REAL        NOT NULL,
    round_trip_cost_bps    REAL        NOT NULL,
    calibrator             TEXT        NOT NULL,
    recipe_version         TEXT        NOT NULL,
    aligned_missing        INTEGER     NOT NULL,   -- sources absent at T0 (their columns NaN)
    serving_role           TEXT        NOT NULL DEFAULT 'ACTIVE'
        CHECK (serving_role IN ('ACTIVE', 'SHADOW')),
    computed_at            TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (instrument_id, bar_open_time, bar_revision, model_id)
) PARTITION BY RANGE (bar_open_time);
-- Schema 3 (cost model v2, policy-2): the decision cost's spread as observed
-- at T0 (or the fallback, spread_observed = false), the model's validated
-- margin (NULL: none, never actionable) and the long-side magnitudes.
ALTER TABLE altml.model_predictions ADD COLUMN IF NOT EXISTS cost_model_version TEXT;
ALTER TABLE altml.model_predictions ADD COLUMN IF NOT EXISTS spread_bps REAL;
ALTER TABLE altml.model_predictions ADD COLUMN IF NOT EXISTS spread_observed BOOLEAN;
ALTER TABLE altml.model_predictions ADD COLUMN IF NOT EXISTS policy_margin_bps REAL;
ALTER TABLE altml.model_predictions ADD COLUMN IF NOT EXISTS expected_mfe_long_bps REAL;
ALTER TABLE altml.model_predictions ADD COLUMN IF NOT EXISTS expected_mae_long_bps REAL;
-- Prediction schema 4 (2026-09-26): the published magnitudes (from the
-- magnitude regressions or the class means, magnitude_source says which) and
-- the calibration verdict the probabilities were published under.
ALTER TABLE altml.model_predictions ADD COLUMN IF NOT EXISTS expected_up_excursion_bps REAL;
ALTER TABLE altml.model_predictions ADD COLUMN IF NOT EXISTS expected_down_excursion_bps REAL;
ALTER TABLE altml.model_predictions ADD COLUMN IF NOT EXISTS magnitude_source TEXT;
ALTER TABLE altml.model_predictions ADD COLUMN IF NOT EXISTS probability_calibrated BOOLEAN;

CREATE TABLE IF NOT EXISTS altml.model_events (
    id          BIGINT PRIMARY KEY GENERATED BY DEFAULT AS IDENTITY,
    model_id    TEXT NOT NULL,
    event       TEXT NOT NULL,        -- trained | validated | approved | activated | rolled_back | retired | rejected
    detail      JSONB NOT NULL DEFAULT '{}',
    actor       TEXT NOT NULL DEFAULT 'system',
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- ---------------------------------------------------------------------------
-- Retention policy and storage observability, mirroring AltTech's pattern:
-- `target_keep`/`minimum_keep` are sized per table for its own purpose, never
-- one blanket number. `cleanup_priority` ascending = dropped first when the
-- database approaches its budget (see storage.pg_hard_limit_gb).
-- ---------------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS altml.retention_policies (
    table_name       TEXT PRIMARY KEY,
    target_keep      INTERVAL NOT NULL,
    minimum_keep     INTERVAL NOT NULL,
    cleanup_priority INTEGER NOT NULL,
    protected        BOOLEAN NOT NULL DEFAULT false,
    rationale        TEXT NOT NULL DEFAULT '',
    updated_at       TIMESTAMPTZ NOT NULL DEFAULT now(),
    CONSTRAINT chk_target_ge_min CHECK (target_keep >= minimum_keep)
);

INSERT INTO altml.retention_policies
    (table_name, target_keep, minimum_keep, cleanup_priority, protected, rationale)
VALUES
    ('altml.label_computation_runs', interval '30 days',  interval '7 days',   10, false,
     'operational audit trail of the label builder; no training value once expired'),
    ('altml.model_events',           interval '180 days', interval '60 days',  20, true,
     'model lifecycle/promotion audit; low volume, high governance value'),
    -- 45/30, not 150/120. The larger figures were never reachable: at the
    -- measured ~1.24 GB/day of feature vectors, the 60 GB soft budget is hit at
    -- about 48 days, so the storage guard would have been dropping partitions
    -- long before 120 -- and the running database had already been set to 45/30
    -- by hand, leaving the schema file describing a system that did not exist.
    -- What has to hold is target >= training.lookback_days (30), and it does.
    -- 40/30 since 2026-09-18 (was 45/30). Measured on VPS1 after the archive
    -- took every set AltTech publishes (talib-core x4 venues, reference_lite,
    -- both microstructure sets, cross_venue_core, macro-core): 1.2-1.7 GB per
    -- day, not 1.24. At 45 days that is 55-75 GB, above the 60 GB soft budget
    -- in normal operation; at 40 days it is 48-68 GB with the hard limit (66)
    -- catching the top of the range by shedding to the 30-day minimum. The
    -- training lookback (30) and its fold margin are untouched.
    ('altml.feature_vectors',        interval '40 days', interval '30 days', 30, true,
     'training substrate: rolling lookback is 30 days (training.lookback_days); 40 kept for walk-forward fold margin'),
    ('altml.labels',                 interval '40 days', interval '30 days', 40, true,
     'paired 1:1 with feature_vectors in every training join; same horizon'),
    ('altml.model_predictions',      interval '60 days', interval '40 days', 25, false,
     'what each econ model said, per bar; outlives the feature window so live estimates can be judged against outcomes')
ON CONFLICT (table_name) DO NOTHING;
-- The running database keeps its own row; a policy change is applied to it
-- explicitly (the ON CONFLICT above never overwrites an operator's value):
UPDATE altml.retention_policies SET target_keep = interval '40 days', updated_at = now()
 WHERE table_name IN ('altml.feature_vectors', 'altml.labels') AND target_keep = interval '45 days';

CREATE TABLE IF NOT EXISTS altml.retention_runs (
    id                 BIGINT PRIMARY KEY GENERATED BY DEFAULT AS IDENTITY,
    started_at         TIMESTAMPTZ NOT NULL DEFAULT now(),
    finished_at        TIMESTAMPTZ,
    mode               TEXT NOT NULL CHECK (mode IN ('scheduled','manual')),
    db_bytes_before     BIGINT NOT NULL,
    db_bytes_after      BIGINT,
    partitions_dropped  INTEGER NOT NULL DEFAULT 0,
    rows_deleted        BIGINT NOT NULL DEFAULT 0,
    detail              JSONB NOT NULL DEFAULT '{}',
    error               TEXT
);

CREATE TABLE IF NOT EXISTS altml.storage_snapshots (
    captured_at    TIMESTAMPTZ PRIMARY KEY DEFAULT now(),
    db_bytes       BIGINT NOT NULL,
    soft_budget_bytes BIGINT NOT NULL,
    hard_limit_bytes  BIGINT NOT NULL,
    state          TEXT NOT NULL CHECK (state IN ('NORMAL','WARNING','CRITICAL','HARD_LIMIT'))
);
