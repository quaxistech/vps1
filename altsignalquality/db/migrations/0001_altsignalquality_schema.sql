-- AltSignalQuality schema — the learned signal filter between AltStrategy and VPS2.
--
-- PLACEMENT. This schema lives INSIDE the shared AltSpotter database, next to
-- the `altstrategy` schema, for the same reason AltStrategy does: every read
-- this service makes joins its own rows to AltStrategy's proposals, contexts,
-- training examples and market outcomes. A separate database would put a
-- postgres_fdw hop on the hot path of both inference and every training cycle
-- (see the stack's FDW experience: a correlated foreign join without
-- use_remote_estimate pulls whole tables). One DSN, local joins, no FDW.
--
-- OWNERSHIP. AltSignalQuality owns exactly the `altsignalquality` schema. It
-- never creates, alters or drops anything in `altstrategy`, `public`
-- (AltSpotter) or any other schema; it reads AltStrategy's tables read-only via
-- plain SELECT grants. It does NOT restate AltStrategy's outcomes: market
-- outcomes and their maturation remain AltStrategy's, and are referenced by
-- value from the training examples built here.
--
-- Conventions (shared with altstrategy/altml):
--   * every timestamp column is TIMESTAMPTZ in UTC; wire times stay BIGINT ms;
--   * every id is deterministic in its natural key, so a NATS redelivery or a
--     restart reproduces the same row instead of a duplicate;
--   * writes are ON CONFLICT DO NOTHING / DO UPDATE;
--   * retention is per-table and time-based, driven by altsignalquality.retention_policies.

CREATE SCHEMA IF NOT EXISTS altsignalquality;

-- =========================================================================
-- Versioned contracts: feature schema and label spec.
--
-- A model is only interpretable against the exact ordered feature list and the
-- exact label definition it was trained under. Both are registered here by the
-- service at startup (the C++ contract is the source of truth; this table is
-- its audited, queryable projection) and referenced by every model, evaluation
-- and training example.
-- =========================================================================

CREATE TABLE IF NOT EXISTS altsignalquality.feature_schemas (
    feature_schema_version TEXT PRIMARY KEY,          -- e.g. 'sq-core:1'
    schema_hash            TEXT NOT NULL,             -- sha256 over the ordered contract
    feature_count          INTEGER NOT NULL,
    -- Ordered [{position, name, kind, source, availability}]. `availability`
    -- must be AT_SIGNAL_TIME for every member; anything else is leakage.
    features               JSONB NOT NULL,
    created_at             TIMESTAMPTZ NOT NULL DEFAULT now(),
    CONSTRAINT chk_feature_count_positive CHECK (feature_count > 0)
);

CREATE TABLE IF NOT EXISTS altsignalquality.label_specs (
    label_spec_version TEXT PRIMARY KEY,              -- e.g. 'sq-label:1'
    label_spec_hash    TEXT NOT NULL,
    -- The full, machine-readable label rule: which AltStrategy categorical
    -- outcomes are usable, and what makes the binary label 1.
    definition         JSONB NOT NULL,
    created_at         TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- =========================================================================
-- Live evaluations — one row per candidate signal ever seen.
--
-- This table is the idempotency anchor: `signal_proposal_id` (AltStrategy's own
-- id) is the primary key, so a redelivered candidate can never produce a second
-- evaluation or a second final signal. It is also the point-in-time feature
-- record: the vector stored here is literally the vector the model was fed at
-- T0, which is what makes the training set free of reconstruction drift.
-- =========================================================================

CREATE TABLE IF NOT EXISTS altsignalquality.signal_evaluations (
    signal_proposal_id     TEXT PRIMARY KEY,
    evaluation_id          TEXT NOT NULL UNIQUE,
    candidate_id           TEXT NOT NULL,
    canonical_pair_id      BIGINT NOT NULL,
    symbol                 TEXT NOT NULL DEFAULT '',
    strategy_id            TEXT NOT NULL,
    strategy_version       TEXT NOT NULL DEFAULT '',
    parameter_set_id       TEXT NOT NULL DEFAULT '',
    side                   TEXT NOT NULL CHECK (side IN ('long','short')),
    proposal_grade         TEXT NOT NULL,
    upstream_recommended   BOOLEAN NOT NULL DEFAULT false,
    regime                 TEXT NOT NULL DEFAULT '',

    context_id             TEXT NOT NULL DEFAULT '',
    context_revision       BIGINT NOT NULL DEFAULT 0,
    signal_created_at_ms   BIGINT NOT NULL,           -- T0
    evaluated_at_ms        BIGINT NOT NULL,

    feature_schema_version TEXT NOT NULL,
    feature_schema_hash    TEXT NOT NULL,
    features               REAL[] NOT NULL,           -- ordered, contract-positional
    validity_bitmap        BYTEA NOT NULL,            -- bit i set => features[i] observed
    missing_feature_count  INTEGER NOT NULL DEFAULT 0,

    has_quality_score      BOOLEAN NOT NULL DEFAULT false,
    raw_score              DOUBLE PRECISION,
    quality_score          DOUBLE PRECISION,
    decision_threshold     DOUBLE PRECISION,
    decision               TEXT NOT NULL CHECK (decision IN ('ACCEPTED','REJECTED')),
    decision_basis         TEXT NOT NULL
        CHECK (decision_basis IN ('MODEL','COLD_START','MODEL_UNUSABLE',
                                  'UPSTREAM_NOT_RECOMMENDED','MALFORMED_SIGNAL',
                                  'SCORE_BELOW_FLOOR','SIGNAL_STALE')),
    reason_codes           TEXT[] NOT NULL DEFAULT '{}',
    model_id               TEXT,
    calibration_version    TEXT,
    inference_latency_us   BIGINT NOT NULL DEFAULT 0,

    -- Publication is a two-phase claim: the row is written first (published =
    -- false), the FINAL signal is published, then this flips. A crash between
    -- the two leaves published = false, and the redelivery re-publishes under
    -- the same deterministic final_signal_id, which JetStream de-duplicates.
    final_signal_id        TEXT,
    published              BOOLEAN NOT NULL DEFAULT false,
    published_at_ms        BIGINT,
    -- The candidate's ORIGINAL wire bytes, kept only while an accepted signal
    -- is unpublished. A recovery re-publish must carry the proposal AltStrategy
    -- actually sent, byte for byte — rebuilding it from the archive would
    -- reconstruct an approximation of the trade, and a downstream execution
    -- layer would act on the approximation. Cleared once published, so the
    -- column costs nothing in steady state.
    candidate_payload      BYTEA,

    created_at             TIMESTAMPTZ NOT NULL DEFAULT now(),
    CONSTRAINT chk_score_range CHECK (
        quality_score IS NULL OR (quality_score >= 0.0 AND quality_score <= 1.0)),
    CONSTRAINT chk_scored_has_model CHECK (
        NOT has_quality_score OR (model_id IS NOT NULL AND quality_score IS NOT NULL)),
    CONSTRAINT chk_accepted_has_final CHECK (
        decision <> 'ACCEPTED' OR final_signal_id IS NOT NULL)
);
CREATE INDEX IF NOT EXISTS idx_sq_eval_created
    ON altsignalquality.signal_evaluations (signal_created_at_ms DESC);
CREATE INDEX IF NOT EXISTS idx_sq_eval_unpublished
    ON altsignalquality.signal_evaluations (signal_proposal_id)
    WHERE decision = 'ACCEPTED' AND NOT published;
CREATE INDEX IF NOT EXISTS idx_sq_eval_schema
    ON altsignalquality.signal_evaluations (feature_schema_version, signal_created_at_ms);

-- =========================================================================
-- Training examples — an evaluation whose AltStrategy outcome has matured.
--
-- Immutable and reproducible: keyed by (signal, feature schema, label spec), so
-- a new feature schema or label spec produces NEW rows and never rewrites the
-- history a shipped model was trained on. `sequence` is the monotone dataset
-- watermark, mirroring altstrategy.signal_training_examples.
-- =========================================================================

CREATE TABLE IF NOT EXISTS altsignalquality.training_examples (
    example_id             TEXT PRIMARY KEY,
    signal_proposal_id     TEXT NOT NULL,
    candidate_id           TEXT NOT NULL,
    canonical_pair_id      BIGINT NOT NULL,
    symbol                 TEXT NOT NULL DEFAULT '',
    strategy_id            TEXT NOT NULL,
    strategy_version       TEXT NOT NULL DEFAULT '',
    parameter_set_id       TEXT NOT NULL DEFAULT '',
    side                   TEXT NOT NULL CHECK (side IN ('long','short')),
    proposal_grade         TEXT NOT NULL,
    upstream_recommended   BOOLEAN NOT NULL DEFAULT false,
    regime                 TEXT NOT NULL DEFAULT '',

    -- Point-in-time anchors. observation_time_ms is T0: no feature value in
    -- this row may depend on anything after it. label_matured_at_ms is when the
    -- outcome became knowable, and is what every purge boundary is computed on.
    observation_time_ms    BIGINT NOT NULL,
    available_at_ms        BIGINT NOT NULL,
    label_matured_at_ms    BIGINT NOT NULL,

    feature_schema_version TEXT NOT NULL REFERENCES altsignalquality.feature_schemas(feature_schema_version),
    feature_schema_hash    TEXT NOT NULL,
    features               REAL[] NOT NULL,
    validity_bitmap        BYTEA NOT NULL,

    label_spec_version     TEXT NOT NULL REFERENCES altsignalquality.label_specs(label_spec_version),
    label                  SMALLINT NOT NULL CHECK (label IN (0,1)),
    categorical_outcome    TEXT NOT NULL,             -- AltStrategy's categorical label
    net_return_after_costs DOUBLE PRECISION NOT NULL,
    mfe                    DOUBLE PRECISION,
    mae                    DOUBLE PRECISION,
    holding_duration_ms    BIGINT,

    -- Provenance back into AltStrategy's own archive (never a copy of it).
    altstrategy_example_id TEXT,
    -- LIVE: features are the exact vector this service scored at T0.
    -- BACKFILL: features were assembled by the same contract from AltStrategy's
    --           point-in-time archive for signals that predate this service.
    origin                 TEXT NOT NULL CHECK (origin IN ('LIVE','BACKFILL')),
    dataset_eligible       BOOLEAN NOT NULL DEFAULT true,
    sequence               BIGSERIAL,
    created_at             TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (signal_proposal_id, feature_schema_version, label_spec_version),
    CONSTRAINT chk_label_after_observation CHECK (label_matured_at_ms > observation_time_ms)
);
CREATE INDEX IF NOT EXISTS idx_sq_example_seq
    ON altsignalquality.training_examples (sequence);
CREATE INDEX IF NOT EXISTS idx_sq_example_training
    ON altsignalquality.training_examples
       (feature_schema_version, label_spec_version, observation_time_ms)
    WHERE dataset_eligible;

-- =========================================================================
-- As-of outcome priors — "how has this strategy / pair / regime behaved so
-- far?", the historical-behaviour block of the feature contract.
--
-- Every counter here aggregates ONLY outcomes that had already matured, so a
-- row read at time T reflects exactly what was knowable at T. The dataset
-- builder reproduces the same numbers by replaying maturation events in time
-- order (domain/priors.hpp), so training and serving share one definition.
-- =========================================================================

CREATE TABLE IF NOT EXISTS altsignalquality.outcome_priors (
    scope_kind         TEXT NOT NULL
        CHECK (scope_kind IN ('GLOBAL','STRATEGY','PAIR','REGIME','STRATEGY_REGIME',
                              'STRATEGY_SIDE','GRADE')),
    scope_key          TEXT NOT NULL,
    sample_count       BIGINT NOT NULL DEFAULT 0,
    win_count          BIGINT NOT NULL DEFAULT 0,
    sum_net_return     DOUBLE PRECISION NOT NULL DEFAULT 0,
    decayed_count      DOUBLE PRECISION NOT NULL DEFAULT 0,
    decayed_wins       DOUBLE PRECISION NOT NULL DEFAULT 0,
    decayed_return     DOUBLE PRECISION NOT NULL DEFAULT 0,
    last_matured_at_ms BIGINT NOT NULL DEFAULT 0,
    updated_at         TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (scope_kind, scope_key)
);

-- The watermark of the prior refresh: outcomes matured at or before this point
-- are already folded into outcome_priors. Refresh is therefore incremental and
-- restart-safe.
CREATE TABLE IF NOT EXISTS altsignalquality.prior_state (
    singleton              BOOLEAN PRIMARY KEY DEFAULT true CHECK (singleton),
    matured_watermark_ms   BIGINT NOT NULL DEFAULT 0,
    half_life_days         DOUBLE PRECISION NOT NULL DEFAULT 30.0,
    decay_anchor_ms        BIGINT NOT NULL DEFAULT 0,
    updated_at             TIMESTAMPTZ NOT NULL DEFAULT now()
);
INSERT INTO altsignalquality.prior_state (singleton) VALUES (true)
ON CONFLICT (singleton) DO NOTHING;

-- =========================================================================
-- Datasets and the model registry.
--
-- Same shape as altml's registry (models / artifacts / metrics /
-- walk_forward_runs / feature_compatibility / events), extended with the two
-- things a FILTER needs that a forecaster does not: the decision threshold and
-- the calibration that makes a score comparable across models.
-- =========================================================================

CREATE TABLE IF NOT EXISTS altsignalquality.dataset_versions (
    dataset_version        TEXT PRIMARY KEY,          -- content-addressed
    feature_schema_version TEXT NOT NULL,
    label_spec_version     TEXT NOT NULL,
    row_count              BIGINT NOT NULL,
    positive_count         BIGINT NOT NULL,
    from_time_ms           BIGINT NOT NULL,
    to_time_ms             BIGINT NOT NULL,
    content_hash           TEXT NOT NULL,             -- sha256 over the ordered example ids
    created_at             TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS altsignalquality.models (
    id                     BIGINT PRIMARY KEY GENERATED BY DEFAULT AS IDENTITY,
    model_id               TEXT NOT NULL UNIQUE,      -- 20260823T030000Z-lightgbm-a1b2c3d4
    model_version          INTEGER NOT NULL DEFAULT 1,
    algorithm              TEXT NOT NULL,             -- lightgbm
    objective              TEXT NOT NULL,             -- binary:signal_quality
    feature_schema_version TEXT NOT NULL,
    feature_schema_hash    TEXT NOT NULL,
    label_spec_version     TEXT NOT NULL,
    dataset_version        TEXT NOT NULL,
    hyperparameters        JSONB NOT NULL DEFAULT '{}',
    decision_threshold     DOUBLE PRECISION NOT NULL DEFAULT 0.5,
    calibration_version    TEXT NOT NULL DEFAULT '',
    training_period        TSTZRANGE,
    validation_period      TSTZRANGE,
    test_period            TSTZRANGE,
    status                 TEXT NOT NULL DEFAULT 'TRAINING'
        CHECK (status IN ('TRAINING','FAILED','CANDIDATE','VALIDATED','APPROVED',
                          'ACTIVE','REJECTED','RETIRED','ROLLBACK')),
    created_at             TIMESTAMPTZ NOT NULL DEFAULT now(),
    approved_at            TIMESTAMPTZ,
    activated_at           TIMESTAMPTZ,
    retired_at             TIMESTAMPTZ,
    CONSTRAINT chk_threshold_range CHECK (decision_threshold >= 0.0 AND decision_threshold <= 1.0)
);

-- Exactly one ACTIVE model per objective. This is what makes "which model is
-- production" a fact rather than a convention.
CREATE UNIQUE INDEX IF NOT EXISTS uq_sq_models_active_per_objective
    ON altsignalquality.models (objective) WHERE status = 'ACTIVE';

CREATE TABLE IF NOT EXISTS altsignalquality.model_artifacts (
    id          BIGINT PRIMARY KEY GENERATED BY DEFAULT AS IDENTITY,
    model_id    TEXT NOT NULL REFERENCES altsignalquality.models(model_id),
    kind        TEXT NOT NULL,     -- model | feature_names | calibration | sample_predictions | report
    path        TEXT NOT NULL,
    format      TEXT NOT NULL,     -- lightgbm-text | json
    checksum    TEXT NOT NULL,     -- sha256
    size_bytes  BIGINT NOT NULL,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (model_id, kind)
);

CREATE TABLE IF NOT EXISTS altsignalquality.model_metrics (
    id          BIGINT PRIMARY KEY GENERATED BY DEFAULT AS IDENTITY,
    model_id    TEXT NOT NULL REFERENCES altsignalquality.models(model_id),
    scope       TEXT NOT NULL,     -- validation | test | test_baseline | fold:<n>
    metric      TEXT NOT NULL,
    value       DOUBLE PRECISION NOT NULL,
    UNIQUE (model_id, scope, metric)
);

CREATE TABLE IF NOT EXISTS altsignalquality.model_walk_forward_runs (
    id          BIGINT PRIMARY KEY GENERATED BY DEFAULT AS IDENTITY,
    model_id    TEXT NOT NULL REFERENCES altsignalquality.models(model_id),
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

CREATE TABLE IF NOT EXISTS altsignalquality.model_feature_compatibility (
    model_id               TEXT NOT NULL REFERENCES altsignalquality.models(model_id),
    feature_schema_version TEXT NOT NULL,
    schema_hash            TEXT NOT NULL,
    feature_count          INTEGER NOT NULL,
    PRIMARY KEY (model_id, feature_schema_version)
);

-- The calibration that turns the booster's raw output into a probability a
-- human threshold can be reasoned about. Versioned separately from the model
-- because it is a separate fit on a separate slice.
CREATE TABLE IF NOT EXISTS altsignalquality.model_calibration (
    model_id            TEXT PRIMARY KEY REFERENCES altsignalquality.models(model_id),
    calibration_version TEXT NOT NULL,
    method              TEXT NOT NULL CHECK (method IN ('isotonic','identity')),
    knots               JSONB NOT NULL,   -- {"x":[...],"y":[...]} monotone step function
    metrics             JSONB NOT NULL DEFAULT '{}',  -- brier/ECE before and after
    created_at          TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS altsignalquality.model_events (
    id          BIGINT PRIMARY KEY GENERATED BY DEFAULT AS IDENTITY,
    model_id    TEXT NOT NULL,
    event       TEXT NOT NULL,     -- trained | rejected | approved | activated | retired | rolled_back
    detail      JSONB NOT NULL DEFAULT '{}',
    actor       TEXT NOT NULL DEFAULT 'system',
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS altsignalquality.training_runs (
    run_id       TEXT PRIMARY KEY,
    started_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    finished_at  TIMESTAMPTZ,
    trigger      TEXT NOT NULL CHECK (trigger IN ('bootstrap','scheduled','manual')),
    status       TEXT NOT NULL DEFAULT 'running'
        CHECK (status IN ('running','succeeded','rejected','failed','deferred')),
    model_id     TEXT,
    rows_used    BIGINT NOT NULL DEFAULT 0,
    detail       JSONB NOT NULL DEFAULT '{}',
    error        TEXT
);

-- =========================================================================
-- Retention. Sized per table for its own purpose, like altstrategy/altml.
-- =========================================================================

CREATE TABLE IF NOT EXISTS altsignalquality.retention_policies (
    table_name   TEXT PRIMARY KEY,
    retain_days  INTEGER NOT NULL CHECK (retain_days > 0),
    time_column  TEXT NOT NULL,
    is_critical  BOOLEAN NOT NULL DEFAULT false,
    rationale    TEXT NOT NULL DEFAULT '',
    updated_at   TIMESTAMPTZ NOT NULL DEFAULT now()
);

INSERT INTO altsignalquality.retention_policies
    (table_name, retain_days, time_column, is_critical, rationale)
VALUES
    ('signal_evaluations', 120, 'created_at', false,
     'live decision audit; a matured evaluation has already become a training example'),
    ('training_examples',  400, 'created_at', true,
     'the training substrate itself: a rolling window well beyond training.lookback_days'),
    ('model_events',       365, 'created_at', true,
     'model lifecycle/promotion audit; tiny volume, high governance value'),
    ('training_runs',       90, 'started_at', false,
     'operational audit of the trainer; superseded by the model rows it produced')
ON CONFLICT (table_name) DO NOTHING;

CREATE TABLE IF NOT EXISTS altsignalquality.retention_runs (
    id            BIGINT PRIMARY KEY GENERATED BY DEFAULT AS IDENTITY,
    started_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
    finished_at   TIMESTAMPTZ,
    rows_deleted  BIGINT NOT NULL DEFAULT 0,
    detail        JSONB NOT NULL DEFAULT '{}',
    error         TEXT
);
