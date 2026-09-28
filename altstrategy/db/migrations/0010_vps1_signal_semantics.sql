-- VPS1 signal semantics (vps1-signal-1): a signal is an EVENT plus the
-- market's expected behaviour after it, recorded whatever any economic
-- estimate says. Everything here is additive.
--
-- 1. The setup's measured description. What a detector used to require on top
--    of its own definition (room to the next zone, venue agreement, volume,
--    a regime) is recorded here instead of gating the setup.
ALTER TABLE altstrategy.strategy_candidates ADD COLUMN IF NOT EXISTS descriptors JSONB;

-- 2. AltML's prediction on EVERY served horizon, per candidate. `state` says
--    whether numbers exist (PRESENT / INCOMPLETE / STALE) or why not
--    (ABSTAIN / INCOMPATIBLE / MISSING); numbers are NULL when absent.
--    Children of strategy_candidates by FOREIGN KEY ... ON DELETE CASCADE:
--    retention of the parent removes them (0008), no policy of their own.
CREATE TABLE IF NOT EXISTS altstrategy.candidate_predictions (
    candidate_id                 TEXT NOT NULL REFERENCES altstrategy.strategy_candidates(candidate_id)
                                     ON DELETE CASCADE,
    horizon_minutes              INTEGER NOT NULL,
    state                        TEXT NOT NULL,
    model_id                     TEXT,
    model_version                INTEGER,
    model_role                   TEXT,
    calibration_version          TEXT,
    probability_calibrated       BOOLEAN NOT NULL DEFAULT false,
    p_up                         DOUBLE PRECISION,
    p_flat                       DOUBLE PRECISION,
    p_down                       DOUBLE PRECISION,
    expected_return_bps          DOUBLE PRECISION,
    expected_up_excursion_bps    DOUBLE PRECISION,
    expected_down_excursion_bps  DOUBLE PRECISION,
    bar_open_time_ms             BIGINT,
    computed_at_ms               BIGINT,
    aligned_missing              INTEGER,
    cost_model_version           TEXT,          -- economic DIAGNOSTIC (never a gate)
    cost_bps                     DOUBLE PRECISION,
    expected_net_long_bps        DOUBLE PRECISION,
    expected_net_short_bps       DOUBLE PRECISION,
    PRIMARY KEY (candidate_id, horizon_minutes)
);

-- 3. When the extremes happened. Market outcome, not execution outcome: the
--    minutes from the reference close to the most favourable and to the most
--    adverse price over the horizon (1-based, the first bar after the close
--    is minute 1).
ALTER TABLE altstrategy.candidate_market_outcomes ADD COLUMN IF NOT EXISTS t_mfe_min INTEGER;
ALTER TABLE altstrategy.candidate_market_outcomes ADD COLUMN IF NOT EXISTS t_mae_min INTEGER;
