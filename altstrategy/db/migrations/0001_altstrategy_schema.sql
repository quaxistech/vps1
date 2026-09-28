-- AltStrategy adaptive decision engine — owned schema (spec §43, §44, §45).
--
-- AltStrategy owns ONLY the `altstrategy` schema. It never modifies the
-- AltSpotter / AltTech / AltML schemas (those live in `public` and their own
-- namespaces and are read-only inputs). Every entity carries an immutable
-- string id and, where NATS redelivery / restart can replay a producer step, a
-- UNIQUE natural key so writes are idempotent.
--
-- altstrategy.v1 wire contract ⇄ these tables. Nothing here stores position size,
-- leverage, account balance or exchange order ids.

CREATE SCHEMA IF NOT EXISTS altstrategy;

-- =========================================================================
-- Strategy catalogue, versions, parameter schemas/sets, lifecycle (§10, §13)
-- =========================================================================

CREATE TABLE IF NOT EXISTS altstrategy.strategy_definitions (
    strategy_id       TEXT PRIMARY KEY,
    family            TEXT NOT NULL,
    supported_sides   TEXT[] NOT NULL DEFAULT '{}',
    supported_horizons TEXT[] NOT NULL DEFAULT '{}',
    holding_classes   TEXT[] NOT NULL DEFAULT '{}',
    allowed_regimes   TEXT[] NOT NULL DEFAULT '{}',
    forbidden_regimes TEXT[] NOT NULL DEFAULT '{}',
    explanation_template TEXT NOT NULL DEFAULT '',
    created_at        TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS altstrategy.strategy_versions (
    strategy_id       TEXT NOT NULL REFERENCES altstrategy.strategy_definitions(strategy_id),
    strategy_version  TEXT NOT NULL,
    required_features TEXT[] NOT NULL DEFAULT '{}',
    required_quality  TEXT NOT NULL DEFAULT 'DEGRADED_2_OF_4',
    max_candidate_freq_ms BIGINT NOT NULL DEFAULT 60000,
    min_history_bars  INT NOT NULL DEFAULT 210,
    cooldown_ms       BIGINT NOT NULL DEFAULT 0,
    default_parameter_set_id TEXT,
    created_at        TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (strategy_id, strategy_version)
);

CREATE TABLE IF NOT EXISTS altstrategy.strategy_parameter_schemas (
    strategy_id       TEXT NOT NULL,
    strategy_version  TEXT NOT NULL,
    param_name        TEXT NOT NULL,
    param_type        TEXT NOT NULL DEFAULT 'double',
    min_value         DOUBLE PRECISION,
    max_value         DOUBLE PRECISION,
    step_value        DOUBLE PRECISION,
    default_value     DOUBLE PRECISION,
    dependency_rules  JSONB NOT NULL DEFAULT '{}'::jsonb,
    PRIMARY KEY (strategy_id, strategy_version, param_name)
);

CREATE TABLE IF NOT EXISTS altstrategy.strategy_parameter_sets (
    parameter_set_id  TEXT PRIMARY KEY,
    strategy_id       TEXT NOT NULL,
    strategy_version  TEXT NOT NULL,
    param_values      JSONB NOT NULL DEFAULT '{}'::jsonb,
    status            TEXT NOT NULL DEFAULT 'offline'
                      CHECK (status IN ('offline','walk_forward','shadow','active','rolled_back','retired')),
    derived_from      TEXT,
    walk_forward_ref  TEXT,
    created_at        TIMESTAMPTZ NOT NULL DEFAULT now(),
    activated_at      TIMESTAMPTZ
);
CREATE INDEX IF NOT EXISTS idx_param_sets_strategy
    ON altstrategy.strategy_parameter_sets (strategy_id, strategy_version, status);

CREATE TABLE IF NOT EXISTS altstrategy.strategy_lifecycle (
    strategy_id       TEXT PRIMARY KEY,
    strategy_version  TEXT NOT NULL,
    lifecycle_status  TEXT NOT NULL
                      CHECK (lifecycle_status IN
                        ('RESEARCH','BACKTESTED','SHADOW','CANDIDATE','LIMITED','ACTIVE','DEGRADED','DISABLED','RETIRED')),
    updated_at        TIMESTAMPTZ NOT NULL DEFAULT now(),
    cooldown_until    TIMESTAMPTZ
);

-- =========================================================================
-- Tier 1: candidates + reasons + ensemble snapshots (§8.1, §15)
-- =========================================================================

CREATE TABLE IF NOT EXISTS altstrategy.strategy_candidates (
    candidate_id      TEXT PRIMARY KEY,
    natural_key       TEXT NOT NULL UNIQUE,          -- idempotency (§45)
    canonical_pair_id BIGINT NOT NULL,
    strategy_id       TEXT NOT NULL,
    strategy_version  TEXT NOT NULL,
    parameter_set_id  TEXT NOT NULL,
    strategy_family   TEXT NOT NULL,
    side              TEXT NOT NULL CHECK (side IN ('long','short')),
    setup_time_ms     BIGINT NOT NULL,
    created_at_ms     BIGINT NOT NULL,
    expires_at_ms     BIGINT NOT NULL,
    context_id        TEXT NOT NULL,
    context_revision  BIGINT NOT NULL,
    raw_setup_score   DOUBLE PRECISION NOT NULL DEFAULT 0,
    static_score      DOUBLE PRECISION NOT NULL DEFAULT 0,
    adaptive_score    DOUBLE PRECISION NOT NULL DEFAULT 0,
    strategy_prior    DOUBLE PRECISION NOT NULL DEFAULT 0,
    strategy_posterior DOUBLE PRECISION NOT NULL DEFAULT 0,
    posterior_uncertainty DOUBLE PRECISION NOT NULL DEFAULT 0,
    effective_sample_size DOUBLE PRECISION NOT NULL DEFAULT 0,
    expected_horizon_ms BIGINT NOT NULL DEFAULT 0,
    holding_class     TEXT NOT NULL DEFAULT 'INTRADAY',
    entry_hint        DOUBLE PRECISION,
    invalidation_hint DOUBLE PRECISION,
    target_hint       DOUBLE PRECISION,
    bitget_regime     TEXT,
    global_regime     TEXT,
    cross_venue_confirmation DOUBLE PRECISION NOT NULL DEFAULT 0,
    selection_mode    TEXT NOT NULL DEFAULT 'EXPLOIT',
    lifecycle_status  TEXT NOT NULL DEFAULT 'ACTIVE',
    quality           TEXT NOT NULL DEFAULT 'STRICT_4_OF_4',
    created_at        TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS idx_candidates_pair_ctx
    ON altstrategy.strategy_candidates (canonical_pair_id, context_id);
CREATE INDEX IF NOT EXISTS idx_candidates_strategy
    ON altstrategy.strategy_candidates (strategy_id, strategy_version, created_at DESC);

CREATE TABLE IF NOT EXISTS altstrategy.strategy_candidate_reasons (
    candidate_id      TEXT NOT NULL REFERENCES altstrategy.strategy_candidates(candidate_id) ON DELETE CASCADE,
    ordinal           INT NOT NULL,
    kind              TEXT NOT NULL DEFAULT 'reason',  -- reason | warning
    code              TEXT NOT NULL,
    PRIMARY KEY (candidate_id, ordinal)
);

CREATE TABLE IF NOT EXISTS altstrategy.strategy_ensemble_snapshots (
    ensemble_id       TEXT PRIMARY KEY,
    context_id        TEXT NOT NULL,
    canonical_pair_id BIGINT NOT NULL,
    ensemble_direction TEXT,
    ensemble_agreement DOUBLE PRECISION NOT NULL DEFAULT 0,
    supporting_strategies TEXT[] NOT NULL DEFAULT '{}',
    supporting_families TEXT[] NOT NULL DEFAULT '{}',
    conflicting_strategies TEXT[] NOT NULL DEFAULT '{}',
    conflicting_families TEXT[] NOT NULL DEFAULT '{}',
    family_independence_score DOUBLE PRECISION NOT NULL DEFAULT 0,
    plan_dispersion   DOUBLE PRECISION NOT NULL DEFAULT 0,
    created_at        TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- =========================================================================
-- Ranking policies + rankings + components (§19, §20)
-- =========================================================================

CREATE TABLE IF NOT EXISTS altstrategy.ranking_policies (
    ranking_policy_id TEXT PRIMARY KEY,
    is_champion       BOOLEAN NOT NULL DEFAULT false,
    active            BOOLEAN NOT NULL DEFAULT true,
    created_at        TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS altstrategy.ranking_policy_versions (
    ranking_policy_id TEXT NOT NULL REFERENCES altstrategy.ranking_policies(ranking_policy_id),
    ranking_policy_version TEXT NOT NULL,
    weights           JSONB NOT NULL DEFAULT '{}'::jsonb,
    normalization_version TEXT NOT NULL DEFAULT 'v1',
    calibration_version TEXT NOT NULL DEFAULT 'v1',
    created_at        TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (ranking_policy_id, ranking_policy_version)
);

CREATE TABLE IF NOT EXISTS altstrategy.strategy_rankings (
    ranking_id        TEXT PRIMARY KEY,
    natural_key       TEXT NOT NULL UNIQUE,
    canonical_pair_id BIGINT NOT NULL,
    context_id        TEXT NOT NULL,
    ranking_policy_id TEXT NOT NULL,
    ranking_policy_version TEXT NOT NULL,
    is_challenger     BOOLEAN NOT NULL DEFAULT false,
    result            TEXT NOT NULL,
    recommended_candidate_id TEXT,
    ensemble_id       TEXT,
    ranking_uncertainty DOUBLE PRECISION NOT NULL DEFAULT 0,
    ranking_regret_estimate DOUBLE PRECISION NOT NULL DEFAULT 0,
    created_at_ms     BIGINT NOT NULL,
    created_at        TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS idx_rankings_ctx
    ON altstrategy.strategy_rankings (context_id, is_challenger);

CREATE TABLE IF NOT EXISTS altstrategy.ranking_components (
    ranking_id        TEXT NOT NULL REFERENCES altstrategy.strategy_rankings(ranking_id) ON DELETE CASCADE,
    candidate_id      TEXT NOT NULL,
    rank              INT NOT NULL,
    final_score       DOUBLE PRECISION NOT NULL,
    adaptive_weight   DOUBLE PRECISION NOT NULL DEFAULT 0,
    posterior_edge    DOUBLE PRECISION NOT NULL DEFAULT 0,
    components        JSONB NOT NULL DEFAULT '{}'::jsonb,
    PRIMARY KEY (ranking_id, candidate_id)
);

-- =========================================================================
-- Trade plans, proposals, warnings, state events (§8.3, §22, §30, §31)
-- =========================================================================

CREATE TABLE IF NOT EXISTS altstrategy.trade_plan_variants (
    variant_id        TEXT PRIMARY KEY,
    natural_key       TEXT NOT NULL UNIQUE,
    candidate_id      TEXT NOT NULL,
    context_id        TEXT NOT NULL,
    canonical_pair_id BIGINT NOT NULL,
    profile           TEXT NOT NULL CHECK (profile IN ('AGGRESSIVE','BALANCED','CONSERVATIVE')),
    side              TEXT NOT NULL CHECK (side IN ('long','short')),
    entry_type        TEXT NOT NULL,
    entry_price       DOUBLE PRECISION,
    entry_lower       DOUBLE PRECISION,
    entry_upper       DOUBLE PRECISION,
    trigger_price     DOUBLE PRECISION,
    stop_loss_price   DOUBLE PRECISION NOT NULL,
    take_profit_price DOUBLE PRECISION NOT NULL,
    optional_secondary_target DOUBLE PRECISION,
    max_holding_time_ms BIGINT NOT NULL,
    time_stop_at_ms   BIGINT NOT NULL,
    expected_fill_time_p50_min DOUBLE PRECISION NOT NULL DEFAULT 0,
    expected_fill_time_p90_min DOUBLE PRECISION NOT NULL DEFAULT 0,
    entry_fill_timeout_ms BIGINT NOT NULL DEFAULT 0,
    stop_distance_bps DOUBLE PRECISION NOT NULL DEFAULT 0,
    stop_distance_atr DOUBLE PRECISION NOT NULL DEFAULT 0,
    target_distance_bps DOUBLE PRECISION NOT NULL DEFAULT 0,
    expected_rr       DOUBLE PRECISION NOT NULL DEFAULT 0,
    stop_basis        TEXT,
    target_basis      TEXT,
    invalidation_reason TEXT,
    created_at        TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS altstrategy.trade_signal_proposals (
    signal_proposal_id TEXT PRIMARY KEY,
    natural_key       TEXT NOT NULL UNIQUE,
    candidate_id      TEXT NOT NULL,
    ranking_id        TEXT,
    canonical_pair_id BIGINT NOT NULL,
    bitget_instrument_id BIGINT NOT NULL,
    strategy_id       TEXT NOT NULL,
    strategy_version  TEXT NOT NULL,
    parameter_set_id  TEXT NOT NULL,
    side              TEXT NOT NULL CHECK (side IN ('long','short')),
    proposal_grade    TEXT NOT NULL
                      CHECK (proposal_grade IN ('STRONG','NORMAL','WEAK','EXPERIMENTAL','SHADOW','INVALID')),
    plan_version      INT NOT NULL DEFAULT 1,
    holding_class     TEXT NOT NULL DEFAULT 'INTRADAY',
    created_at_ms     BIGINT NOT NULL,
    valid_until_ms    BIGINT NOT NULL,
    entry_type        TEXT NOT NULL,
    entry_price       DOUBLE PRECISION,
    entry_lower       DOUBLE PRECISION,
    entry_upper       DOUBLE PRECISION,
    trigger_price     DOUBLE PRECISION,
    max_slippage_bps  DOUBLE PRECISION NOT NULL DEFAULT 0,
    post_only_allowed BOOLEAN NOT NULL DEFAULT false,
    expected_fill_time_p50_min DOUBLE PRECISION NOT NULL DEFAULT 0,
    expected_fill_time_p90_min DOUBLE PRECISION NOT NULL DEFAULT 0,
    entry_fill_timeout_ms BIGINT NOT NULL DEFAULT 0,
    stop_loss_price   DOUBLE PRECISION NOT NULL,
    stop_basis        TEXT,
    stop_distance_bps DOUBLE PRECISION NOT NULL DEFAULT 0,
    stop_distance_atr DOUBLE PRECISION NOT NULL DEFAULT 0,
    take_profit_price DOUBLE PRECISION NOT NULL,
    optional_secondary_target DOUBLE PRECISION,
    target_basis      TEXT,
    target_distance_bps DOUBLE PRECISION NOT NULL DEFAULT 0,
    expected_holding_time_p50_ms BIGINT NOT NULL DEFAULT 0,
    expected_holding_time_p90_ms BIGINT NOT NULL DEFAULT 0,
    max_holding_time_ms BIGINT NOT NULL,
    time_stop_at_ms   BIGINT NOT NULL,
    p_entry_reached   DOUBLE PRECISION,
    p_target_before_stop DOUBLE PRECISION,
    p_stop_before_target DOUBLE PRECISION,
    p_timeout         DOUBLE PRECISION,
    p_ambiguous       DOUBLE PRECISION,
    expected_gross_return DOUBLE PRECISION,
    expected_net_return DOUBLE PRECISION,
    expected_utility  DOUBLE PRECISION,
    expected_rr       DOUBLE PRECISION NOT NULL DEFAULT 0,
    ml_uncertainty    DOUBLE PRECISION,
    ml_ood            DOUBLE PRECISION,
    ml_abstain        BOOLEAN NOT NULL DEFAULT false,
    strategy_confidence DOUBLE PRECISION NOT NULL DEFAULT 0,
    ensemble_agreement DOUBLE PRECISION NOT NULL DEFAULT 0,
    adaptive_score    DOUBLE PRECISION NOT NULL DEFAULT 0,
    quality           TEXT NOT NULL DEFAULT 'STRICT_4_OF_4',
    risk_flags        TEXT[] NOT NULL DEFAULT '{}',
    reason_codes      TEXT[] NOT NULL DEFAULT '{}',
    recommended_for_execution BOOLEAN NOT NULL DEFAULT false,
    alternative_plan_ids TEXT[] NOT NULL DEFAULT '{}',
    context_revision  BIGINT NOT NULL DEFAULT 0,
    feature_manifest_hash TEXT,
    model_versions    TEXT[] NOT NULL DEFAULT '{}',
    ranking_policy_id TEXT,
    state             TEXT NOT NULL DEFAULT 'PROPOSED',
    superseded_by_id  TEXT,
    created_at        TIMESTAMPTZ NOT NULL DEFAULT now(),
    -- Hard price-side invariants (§5.1) enforced at the storage boundary too.
    CONSTRAINT chk_long_geometry CHECK (
        side <> 'long' OR (take_profit_price > entry_price AND stop_loss_price < entry_price)),
    CONSTRAINT chk_short_geometry CHECK (
        side <> 'short' OR (take_profit_price < entry_price AND stop_loss_price > entry_price)),
    CONSTRAINT chk_holding_limit CHECK (max_holding_time_ms <= 172800000)  -- 48h
);
CREATE INDEX IF NOT EXISTS idx_proposals_pair
    ON altstrategy.trade_signal_proposals (canonical_pair_id, created_at DESC);
CREATE INDEX IF NOT EXISTS idx_proposals_state
    ON altstrategy.trade_signal_proposals (state) WHERE state IN ('PROPOSED','OUTCOME_PENDING');

CREATE TABLE IF NOT EXISTS altstrategy.proposal_warnings (
    signal_proposal_id TEXT NOT NULL REFERENCES altstrategy.trade_signal_proposals(signal_proposal_id) ON DELETE CASCADE,
    ordinal           INT NOT NULL,
    code              TEXT NOT NULL,
    detail            TEXT NOT NULL DEFAULT '',
    severity          TEXT NOT NULL DEFAULT 'warning',
    PRIMARY KEY (signal_proposal_id, ordinal)
);

CREATE TABLE IF NOT EXISTS altstrategy.proposal_state_events (
    signal_proposal_id TEXT NOT NULL REFERENCES altstrategy.trade_signal_proposals(signal_proposal_id) ON DELETE CASCADE,
    ordinal           INT NOT NULL,
    from_state        TEXT,
    to_state          TEXT NOT NULL,
    reason            TEXT,
    at_ms             BIGINT NOT NULL,
    PRIMARY KEY (signal_proposal_id, ordinal)
);

-- =========================================================================
-- Learning loop: market/execution/counterfactual outcomes + attribution
-- (§35, §36, §37, §38)
-- =========================================================================

CREATE TABLE IF NOT EXISTS altstrategy.market_outcomes (
    outcome_id        TEXT PRIMARY KEY,
    signal_proposal_id TEXT,
    candidate_id      TEXT NOT NULL,
    canonical_pair_id BIGINT NOT NULL,
    outcome           TEXT NOT NULL
                      CHECK (outcome IN
                        ('ENTRY_NOT_REACHED','ENTRY_EXPIRED','TARGET_FIRST','STOP_FIRST','TIME_STOP','CANCELLED','AMBIGUOUS','DATA_INCOMPLETE')),
    path_source       TEXT NOT NULL DEFAULT 'PATH_1M_CANDLES',
    entry_time_ms     BIGINT,
    resolved_time_ms  BIGINT,
    entry_fill_price  DOUBLE PRECISION,
    exit_price        DOUBLE PRECISION,
    realized_gross_return DOUBLE PRECISION,
    realized_net_return DOUBLE PRECISION,
    mfe               DOUBLE PRECISION,
    mae               DOUBLE PRECISION,
    holding_time_ms   BIGINT,
    ambiguous         BOOLEAN NOT NULL DEFAULT false,
    is_counterfactual BOOLEAN NOT NULL DEFAULT false,
    created_at        TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (candidate_id, is_counterfactual)
);
CREATE INDEX IF NOT EXISTS idx_market_outcomes_proposal
    ON altstrategy.market_outcomes (signal_proposal_id);

CREATE TABLE IF NOT EXISTS altstrategy.execution_feedback (
    signal_proposal_id TEXT PRIMARY KEY REFERENCES altstrategy.trade_signal_proposals(signal_proposal_id) ON DELETE CASCADE,
    accepted          BOOLEAN NOT NULL,
    rejection_reasons TEXT[] NOT NULL DEFAULT '{}',
    order_submitted   BOOLEAN NOT NULL DEFAULT false,
    order_type        TEXT,
    submitted_at_ms   BIGINT,
    fill_price        DOUBLE PRECISION,
    fill_time_ms      BIGINT,
    fill_ratio        DOUBLE PRECISION,
    slippage_bps      DOUBLE PRECISION,
    fees              DOUBLE PRECISION,
    funding           DOUBLE PRECISION,
    position_opened   BOOLEAN NOT NULL DEFAULT false,
    position_closed   BOOLEAN NOT NULL DEFAULT false,
    stop_result       TEXT,
    target_result     TEXT,
    time_stop_result  TEXT,
    realized_pnl      DOUBLE PRECISION,
    mfe               DOUBLE PRECISION,
    mae               DOUBLE PRECISION,
    execution_anomalies TEXT[] NOT NULL DEFAULT '{}',
    received_at_ms    BIGINT NOT NULL,
    created_at        TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS altstrategy.counterfactual_outcomes (
    counterfactual_id TEXT PRIMARY KEY,
    ranking_id        TEXT NOT NULL,
    selected_candidate_id TEXT NOT NULL,
    candidate_id      TEXT NOT NULL,
    outcome           TEXT NOT NULL,
    realized_net_return DOUBLE PRECISION,
    ranking_regret    DOUBLE PRECISION,
    best_hindsight    BOOLEAN NOT NULL DEFAULT false,
    created_at        TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (ranking_id, candidate_id)
);

CREATE TABLE IF NOT EXISTS altstrategy.outcome_attribution (
    attribution_id    TEXT PRIMARY KEY,
    signal_proposal_id TEXT NOT NULL UNIQUE,
    strategy_attribution DOUBLE PRECISION NOT NULL DEFAULT 0,
    ranking_attribution DOUBLE PRECISION NOT NULL DEFAULT 0,
    geometry_attribution DOUBLE PRECISION NOT NULL DEFAULT 0,
    ml_attribution    DOUBLE PRECISION NOT NULL DEFAULT 0,
    execution_attribution DOUBLE PRECISION NOT NULL DEFAULT 0,
    cost_attribution  DOUBLE PRECISION NOT NULL DEFAULT 0,
    regime_attribution DOUBLE PRECISION NOT NULL DEFAULT 0,
    dominant_factor   TEXT,
    created_at        TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- =========================================================================
-- Adaptive stats + posteriors (§16, §17, §18)
-- =========================================================================

CREATE TABLE IF NOT EXISTS altstrategy.strategy_stats_lifetime (
    strategy_id       TEXT NOT NULL,
    strategy_version  TEXT NOT NULL,
    regime            TEXT NOT NULL DEFAULT '*',
    symbol_group      TEXT NOT NULL DEFAULT '*',
    canonical_pair_id BIGINT NOT NULL DEFAULT 0,
    side              TEXT NOT NULL DEFAULT '*',
    holding_class     TEXT NOT NULL DEFAULT '*',
    n                 BIGINT NOT NULL DEFAULT 0,
    wins              BIGINT NOT NULL DEFAULT 0,
    sum_net_return    DOUBLE PRECISION NOT NULL DEFAULT 0,
    sum_sq_net_return DOUBLE PRECISION NOT NULL DEFAULT 0,
    updated_at        TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (strategy_id, strategy_version, regime, symbol_group, canonical_pair_id, side, holding_class)
);

CREATE TABLE IF NOT EXISTS altstrategy.strategy_stats_rolling (
    strategy_id       TEXT NOT NULL,
    strategy_version  TEXT NOT NULL,
    window_kind       TEXT NOT NULL CHECK (window_kind IN ('7d','30d','90d')),
    regime            TEXT NOT NULL DEFAULT '*',
    symbol_group      TEXT NOT NULL DEFAULT '*',
    side              TEXT NOT NULL DEFAULT '*',
    n                 BIGINT NOT NULL DEFAULT 0,
    wins              BIGINT NOT NULL DEFAULT 0,
    sum_net_return    DOUBLE PRECISION NOT NULL DEFAULT 0,
    updated_at        TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (strategy_id, strategy_version, window_kind, regime, symbol_group, side)
);

CREATE TABLE IF NOT EXISTS altstrategy.strategy_stats_decayed (
    strategy_id       TEXT NOT NULL,
    strategy_version  TEXT NOT NULL,
    regime            TEXT NOT NULL DEFAULT '*',
    symbol_group      TEXT NOT NULL DEFAULT '*',
    side              TEXT NOT NULL DEFAULT '*',
    half_life_days    DOUBLE PRECISION NOT NULL DEFAULT 30,
    decayed_n         DOUBLE PRECISION NOT NULL DEFAULT 0,
    decayed_edge      DOUBLE PRECISION NOT NULL DEFAULT 0,
    decayed_var       DOUBLE PRECISION NOT NULL DEFAULT 0,
    last_event_ms     BIGINT NOT NULL DEFAULT 0,
    updated_at        TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (strategy_id, strategy_version, regime, symbol_group, side)
);

CREATE TABLE IF NOT EXISTS altstrategy.strategy_posteriors (
    strategy_id       TEXT NOT NULL,
    strategy_version  TEXT NOT NULL,
    parameter_set_id  TEXT NOT NULL DEFAULT '*',
    regime            TEXT NOT NULL DEFAULT '*',
    symbol_group      TEXT NOT NULL DEFAULT '*',
    canonical_pair_id BIGINT NOT NULL DEFAULT 0,
    side              TEXT NOT NULL DEFAULT '*',
    holding_class     TEXT NOT NULL DEFAULT '*',
    strategy_prior    DOUBLE PRECISION NOT NULL DEFAULT 0,
    posterior_mean_edge DOUBLE PRECISION NOT NULL DEFAULT 0,
    posterior_uncertainty DOUBLE PRECISION NOT NULL DEFAULT 1,
    effective_sample_size DOUBLE PRECISION NOT NULL DEFAULT 0,
    shrinkage_factor  DOUBLE PRECISION NOT NULL DEFAULT 1,
    time_decay_state  DOUBLE PRECISION NOT NULL DEFAULT 0,
    adaptive_weight   DOUBLE PRECISION NOT NULL DEFAULT 1,
    sample_count      BIGINT NOT NULL DEFAULT 0,
    updated_at_ms     BIGINT NOT NULL DEFAULT 0,
    updated_at        TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (strategy_id, strategy_version, parameter_set_id, regime, symbol_group, canonical_pair_id, side, holding_class)
);

-- =========================================================================
-- Drift, calibration, challenger runs, lifecycle events (§39, §33, §20, §40)
-- =========================================================================

CREATE TABLE IF NOT EXISTS altstrategy.strategy_drift_snapshots (
    drift_id          TEXT PRIMARY KEY,
    strategy_id       TEXT NOT NULL,
    strategy_version  TEXT NOT NULL,
    status            TEXT NOT NULL CHECK (status IN ('STABLE','WATCH','DEGRADED','STRUCTURAL_BREAK')),
    candidate_frequency_drift DOUBLE PRECISION NOT NULL DEFAULT 0,
    score_distribution_drift DOUBLE PRECISION NOT NULL DEFAULT 0,
    outcome_drift     DOUBLE PRECISION NOT NULL DEFAULT 0,
    fill_time_drift   DOUBLE PRECISION NOT NULL DEFAULT 0,
    holding_time_drift DOUBLE PRECISION NOT NULL DEFAULT 0,
    mfe_mae_drift     DOUBLE PRECISION NOT NULL DEFAULT 0,
    regime_performance_drift DOUBLE PRECISION NOT NULL DEFAULT 0,
    symbol_performance_drift DOUBLE PRECISION NOT NULL DEFAULT 0,
    reasons           TEXT[] NOT NULL DEFAULT '{}',
    computed_at_ms    BIGINT NOT NULL,
    created_at        TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS idx_drift_strategy
    ON altstrategy.strategy_drift_snapshots (strategy_id, strategy_version, created_at DESC);

CREATE TABLE IF NOT EXISTS altstrategy.parameter_calibration_runs (
    run_id            TEXT PRIMARY KEY,
    strategy_id       TEXT NOT NULL,
    strategy_version  TEXT NOT NULL,
    target_kind       TEXT NOT NULL,   -- stop_buffer | target_quantile | entry_timeout | time_stop | fill_time | slippage
    context_scope     JSONB NOT NULL DEFAULT '{}'::jsonb,
    predicted_vs_actual JSONB NOT NULL DEFAULT '{}'::jsonb,
    proposed_parameter_set_id TEXT,
    status            TEXT NOT NULL DEFAULT 'proposed',
    created_at        TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS altstrategy.ranking_challenger_runs (
    run_id            TEXT PRIMARY KEY,
    champion_policy_id TEXT NOT NULL,
    challenger_policy_id TEXT NOT NULL,
    n_decisions       BIGINT NOT NULL DEFAULT 0,
    champion_regret   DOUBLE PRECISION NOT NULL DEFAULT 0,
    challenger_regret DOUBLE PRECISION NOT NULL DEFAULT 0,
    promotion_ready   BOOLEAN NOT NULL DEFAULT false,
    decision          TEXT NOT NULL DEFAULT 'shadow',  -- shadow | promote | rollback
    created_at        TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS altstrategy.lifecycle_events (
    event_id          TEXT PRIMARY KEY,
    strategy_id       TEXT NOT NULL,
    strategy_version  TEXT NOT NULL,
    from_status       TEXT NOT NULL,
    to_status         TEXT NOT NULL,
    reason            TEXT NOT NULL DEFAULT '',
    sample_count      BIGINT NOT NULL DEFAULT 0,
    rollback          BOOLEAN NOT NULL DEFAULT false,
    policy_version    TEXT NOT NULL DEFAULT 'v1',
    created_at_ms     BIGINT NOT NULL,
    created_at        TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- =========================================================================
-- Operational bookkeeping: computation runs, validation, retention (§41, §52)
-- =========================================================================

CREATE TABLE IF NOT EXISTS altstrategy.computation_runs (
    run_id            TEXT PRIMARY KEY,
    job               TEXT NOT NULL,   -- stats_refresh | posterior_update | drift_scan | calibration | challenger | lifecycle | retention
    started_at        TIMESTAMPTZ NOT NULL DEFAULT now(),
    finished_at       TIMESTAMPTZ,
    status            TEXT NOT NULL DEFAULT 'running',
    detail            JSONB NOT NULL DEFAULT '{}'::jsonb
);

CREATE TABLE IF NOT EXISTS altstrategy.validation_results (
    validation_id     TEXT PRIMARY KEY,
    subject_kind      TEXT NOT NULL,   -- parameter_set | ranking_policy | strategy
    subject_id        TEXT NOT NULL,
    method            TEXT NOT NULL,   -- walk_forward | embargo | out_of_sample | regime_slice | ...
    passed            BOOLEAN NOT NULL DEFAULT false,
    metrics           JSONB NOT NULL DEFAULT '{}'::jsonb,
    created_at        TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS altstrategy.retention_policies (
    table_name        TEXT PRIMARY KEY,
    retain_days       INT NOT NULL,
    time_column       TEXT NOT NULL DEFAULT 'created_at',  -- column the purge job filters on
    is_critical       BOOLEAN NOT NULL DEFAULT false
);

CREATE TABLE IF NOT EXISTS altstrategy.retention_runs (
    run_id            TEXT PRIMARY KEY,
    table_name        TEXT NOT NULL,
    deleted_rows      BIGINT NOT NULL DEFAULT 0,
    ran_at            TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- Retention policy seed is inserted once, at the end of this file (after every
-- table it references has been created) — see "Retention policy defaults" below.

-- =========================================================================
-- Point-in-time context snapshots (§7, §17, §40 point-in-time correctness)
-- =========================================================================

CREATE TABLE IF NOT EXISTS altstrategy.decision_context_snapshots (
    context_id        TEXT PRIMARY KEY,
    canonical_pair_id BIGINT NOT NULL,
    bitget_instrument_id BIGINT NOT NULL,
    context_revision  BIGINT NOT NULL,
    event_time_ms     BIGINT NOT NULL,       -- observation_time
    available_at_ms   BIGINT NOT NULL,       -- when the inputs were usable
    expires_at_ms     BIGINT NOT NULL,
    global_universe_status TEXT NOT NULL,
    data_quality      TEXT NOT NULL,
    venue_mask        INT NOT NULL DEFAULT 0,
    missing_venue_mask INT NOT NULL DEFAULT 0,
    regime_divergence BOOLEAN NOT NULL DEFAULT false,
    bitget_regime     TEXT,
    global_regime     TEXT,
    feature_manifest_hash TEXT NOT NULL,
    label_spec_hash   TEXT,
    producer_versions JSONB NOT NULL DEFAULT '{}'::jsonb,
    fees_bps          DOUBLE PRECISION,
    funding_rate      DOUBLE PRECISION,
    mark_price        DOUBLE PRECISION,
    spread_bps        DOUBLE PRECISION,
    top_depth_usdt    DOUBLE PRECISION,
    liquidity_rank    DOUBLE PRECISION,
    expected_slippage_bps DOUBLE PRECISION,
    -- Immutable full snapshot of the inputs available at creation (§17). Never
    -- overwritten with recomputed values without a new context_revision.
    snapshot          JSONB NOT NULL DEFAULT '{}'::jsonb,
    created_at        TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (canonical_pair_id, context_revision, event_time_ms)
);
CREATE INDEX IF NOT EXISTS idx_ctx_snap_pair
    ON altstrategy.decision_context_snapshots (canonical_pair_id, event_time_ms DESC);

-- =========================================================================
-- SignalTrainingArchive (§23, §24). Mature examples for VPS2 SignalQuality.
-- =========================================================================

CREATE TABLE IF NOT EXISTS altstrategy.signal_training_examples (
    example_id        TEXT PRIMARY KEY,
    signal_proposal_id TEXT NOT NULL,
    candidate_id      TEXT NOT NULL,
    plan_variant_id   TEXT,
    observation_time_ms BIGINT NOT NULL,
    available_at_ms   BIGINT NOT NULL,
    label_matured_at_ms BIGINT,
    canonical_pair_id BIGINT NOT NULL,
    strategy_id       TEXT NOT NULL,
    strategy_version  TEXT NOT NULL,
    parameter_set_id  TEXT NOT NULL,
    side              TEXT NOT NULL,
    proposal_grade    TEXT NOT NULL,
    feature_manifest_hash TEXT NOT NULL,
    label_spec_hash   TEXT NOT NULL,
    quality           TEXT NOT NULL,
    context_id        TEXT NOT NULL,
    plan_geometry     JSONB NOT NULL DEFAULT '{}'::jsonb,
    input_features    JSONB NOT NULL DEFAULT '{}'::jsonb,
    dataset_eligible  BOOLEAN NOT NULL DEFAULT false,
    sequence          BIGSERIAL,             -- incremental snapshot boundary
    created_at        TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (signal_proposal_id, plan_variant_id)
);
CREATE INDEX IF NOT EXISTS idx_training_examples_seq
    ON altstrategy.signal_training_examples (sequence);
CREATE INDEX IF NOT EXISTS idx_training_examples_eligible
    ON altstrategy.signal_training_examples (dataset_eligible, label_matured_at_ms);

CREATE TABLE IF NOT EXISTS altstrategy.signal_training_labels (
    example_id        TEXT PRIMARY KEY REFERENCES altstrategy.signal_training_examples(example_id) ON DELETE CASCADE,
    categorical_label TEXT NOT NULL,         -- TARGET_FIRST / STOP_FIRST / TIME_STOP / ENTRY_* / AMBIGUOUS / DATA_INCOMPLETE
    net_return_after_costs DOUBLE PRECISION,
    mfe               DOUBLE PRECISION,
    mae               DOUBLE PRECISION,
    time_to_entry_ms  BIGINT,
    time_to_target_ms BIGINT,
    time_to_stop_ms   BIGINT,
    holding_duration_ms BIGINT,
    -- AMBIGUOUS / DATA_INCOMPLETE are excluded from binary label training (§24).
    binary_label_usable BOOLEAN NOT NULL DEFAULT true,
    label_spec_hash   TEXT NOT NULL,
    created_at        TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS altstrategy.signal_training_context (
    example_id        TEXT PRIMARY KEY REFERENCES altstrategy.signal_training_examples(example_id) ON DELETE CASCADE,
    context_id        TEXT NOT NULL,
    context_snapshot  JSONB NOT NULL DEFAULT '{}'::jsonb,
    created_at        TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- =========================================================================
-- Training snapshot exports (§26–§31). Immutable, content-addressed.
-- =========================================================================

CREATE TABLE IF NOT EXISTS altstrategy.signal_archive_snapshots (
    snapshot_id       TEXT PRIMARY KEY,
    kind              TEXT NOT NULL CHECK (kind IN ('FULL','INCREMENTAL')),
    cutoff_time_ms    BIGINT NOT NULL,
    from_sequence     BIGINT,
    to_sequence       BIGINT NOT NULL,
    previous_snapshot_id TEXT,
    manifest_hash     TEXT NOT NULL,
    feature_manifest_hashes TEXT[] NOT NULL DEFAULT '{}',
    label_spec_hashes TEXT[] NOT NULL DEFAULT '{}',
    signal_count      BIGINT NOT NULL DEFAULT 0,
    candidate_count   BIGINT NOT NULL DEFAULT 0,
    plan_count        BIGINT NOT NULL DEFAULT 0,
    matured_outcome_count BIGINT NOT NULL DEFAULT 0,
    artifact_location TEXT NOT NULL,         -- /var/lib/altstrategy/exports/...
    format            TEXT NOT NULL DEFAULT 'parquet',
    status            TEXT NOT NULL DEFAULT 'created' CHECK (status IN ('created','superseded','failed')),
    created_at        TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS idx_archive_snap_status
    ON altstrategy.signal_archive_snapshots (status, created_at DESC);

CREATE TABLE IF NOT EXISTS altstrategy.signal_archive_files (
    snapshot_id       TEXT NOT NULL REFERENCES altstrategy.signal_archive_snapshots(snapshot_id) ON DELETE CASCADE,
    file_name         TEXT NOT NULL,
    row_count         BIGINT NOT NULL DEFAULT 0,
    byte_size         BIGINT NOT NULL DEFAULT 0,
    sha256            TEXT NOT NULL,
    PRIMARY KEY (snapshot_id, file_name)
);

CREATE TABLE IF NOT EXISTS altstrategy.signal_archive_exports (
    export_id         TEXT PRIMARY KEY,
    snapshot_id       TEXT NOT NULL,
    started_at        TIMESTAMPTZ NOT NULL DEFAULT now(),
    finished_at       TIMESTAMPTZ,
    status            TEXT NOT NULL DEFAULT 'running',
    detail            JSONB NOT NULL DEFAULT '{}'::jsonb
);

-- =========================================================================
-- Retention policy defaults (§25, §39, §52).
--
-- AltStrategy trades short-horizon: every proposal's hard max holding time is
-- <= 48h (chk_holding_limit above), so raw per-decision detail has no
-- operational value much beyond that horizon. The adaptive ranking loop
-- itself only needs a bounded lookback: strategy_stats_decayed uses an
-- exponential half-life (default 30 days, see decision_types/allocator), so
-- an observation older than ~3 half-lives (~90 days) contributes <15% of its
-- original weight to the live posterior. Long-term ML training history does
-- NOT live in this OLTP database — signal_training_examples/labels/context
-- are staged here only until `signal_archive_snapshots` exports them as
-- immutable, content-addressed Parquet files under
-- /var/lib/altstrategy/exports (see docs/SNAPSHOT_RETENTION.md); once
-- exported, the DB copy is disposable.
--
-- These are DEFAULTS, not hardcoded limits: every value is a plain row in
-- this table and can be changed at any time with
--   UPDATE altstrategy.retention_policies SET retain_days = <n> WHERE table_name = '<t>';
-- with no code change and no restart required (the maintenance scheduler
-- re-reads this table on every retention pass). The values below are sized
-- for the initial ~3 GB test deployment; widen them freely once the
-- production disk budget is sized per docs/OPERATIONS.md.
INSERT INTO altstrategy.retention_policies (table_name, retain_days, time_column, is_critical) VALUES
    -- Strategy catalogue / config: tiny row counts, kept indefinitely by default.
    ('strategy_definitions', 3650, 'created_at', true),
    ('strategy_versions', 3650, 'created_at', true),
    ('strategy_parameter_sets', 3650, 'created_at', true),
    ('ranking_policies', 3650, 'created_at', true),
    ('ranking_policy_versions', 3650, 'created_at', true),
    -- Adaptive statistics: compressed/aggregated in place, not append-only growth.
    ('strategy_posteriors', 3650, 'updated_at', true),
    ('strategy_stats_lifetime', 3650, 'updated_at', true),
    ('strategy_stats_rolling', 3650, 'updated_at', true),
    ('strategy_stats_decayed', 3650, 'updated_at', true),
    -- Per-decision diagnostic detail: only needed near-term; the decayed
    -- statistics above already carry the long-run statistical signal.
    ('strategy_candidates', 14, 'created_at', false),
    -- strategy_candidate_reasons and ranking_components are NOT listed: both are
    -- children keyed ON DELETE CASCADE to the rows above, so they expire with
    -- their parent. They also carry no `created_at` of their own, so a policy on
    -- them could only ever fail -- which it did, daily, since deployment.
    ('strategy_ensemble_snapshots', 14, 'created_at', false),
    ('strategy_rankings', 14, 'created_at', false),
    ('trade_plan_variants', 30, 'created_at', false),
    -- Trade lifecycle & outcomes: the core learning signal. 90 days is ~3x
    -- the posterior half-life and ~45x the 48h max holding time.
    ('trade_signal_proposals', 90, 'created_at', true),
    ('market_outcomes', 90, 'created_at', true),
    ('execution_feedback', 90, 'created_at', true),
    ('counterfactual_outcomes', 90, 'created_at', true),
    ('outcome_attribution', 90, 'created_at', true),
    -- Drift / calibration / challenger bookkeeping.
    ('strategy_drift_snapshots', 180, 'created_at', false),
    ('parameter_calibration_runs', 180, 'created_at', false),
    ('ranking_challenger_runs', 180, 'created_at', false),
    ('lifecycle_events', 3650, 'created_at', true),
    -- Operational bookkeeping: short-lived, purely diagnostic.
    ('computation_runs', 30, 'started_at', false),
    ('validation_results', 180, 'created_at', false),
    ('retention_runs', 30, 'ran_at', false),
    -- Point-in-time context snapshots: the largest per-row payload (JSONB);
    -- only needed for near-term replay/debugging. Matured examples keep
    -- their own copy in signal_training_context for the training window.
    ('decision_context_snapshots', 14, 'created_at', false),
    -- Training archive staging: kept for the training window (matches the
    -- documented 180-day training window in docs/SNAPSHOT_RETENTION.md),
    -- then disposable once exported to the immutable Parquet archive.
    ('signal_training_examples', 180, 'created_at', true),
    ('signal_training_labels', 180, 'created_at', true),
    ('signal_training_context', 180, 'created_at', true),
    -- Archive/export metadata: tiny rows (hashes/paths), the bulk data lives
    -- in exported files, not Postgres.
    ('signal_archive_snapshots', 3650, 'created_at', true),
    ('signal_archive_exports', 365, 'started_at', false)
ON CONFLICT (table_name) DO UPDATE SET
    retain_days = EXCLUDED.retain_days,
    time_column = EXCLUDED.time_column,
    is_critical = EXCLUDED.is_critical;
