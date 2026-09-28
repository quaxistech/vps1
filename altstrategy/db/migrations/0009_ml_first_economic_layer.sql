-- 0009: the ML-first economic decision layer.
--
-- 1. Every candidate records the economic policy's verdict: which horizon was
--    evaluated, what the calibrated model said for this side, against what
--    cost, from which model and which bar. NULL means "not evaluated" (no
--    usable model) and is never read as zero.
ALTER TABLE altstrategy.strategy_candidates
    ADD COLUMN IF NOT EXISTS econ_state              TEXT,
    ADD COLUMN IF NOT EXISTS econ_horizon_minutes    INTEGER,
    ADD COLUMN IF NOT EXISTS econ_expected_net_bps   DOUBLE PRECISION,
    ADD COLUMN IF NOT EXISTS econ_p_side             DOUBLE PRECISION,
    ADD COLUMN IF NOT EXISTS econ_p_opposite         DOUBLE PRECISION,
    ADD COLUMN IF NOT EXISTS econ_round_trip_cost_bps DOUBLE PRECISION,
    ADD COLUMN IF NOT EXISTS econ_model_id           TEXT,
    ADD COLUMN IF NOT EXISTS econ_forecast_bar_ms    BIGINT;

-- 2. The MARKET outcome of every candidate at the three model horizons,
--    whatever its lifecycle and whether or not it was planned: the side-
--    adjusted return of a position opened at the last 1m close before the
--    candidate was raised and held for the horizon. It is not an execution
--    outcome (no limit fill, no stop, no target) -- that line stays in
--    market_outcomes / execution_feedback. Children of the candidate; the
--    candidate's retention removes them.
CREATE TABLE IF NOT EXISTS altstrategy.candidate_market_outcomes (
    candidate_id     TEXT NOT NULL REFERENCES altstrategy.strategy_candidates(candidate_id)
                         ON DELETE CASCADE,
    horizon_minutes  INTEGER NOT NULL,
    entry_ref_ms     BIGINT NOT NULL,            -- close of the reference 1m bar
    entry_price      DOUBLE PRECISION NOT NULL,
    exit_price       DOUBLE PRECISION NOT NULL,
    gross_bps        DOUBLE PRECISION NOT NULL,  -- side-adjusted simple return
    mfe_bps          DOUBLE PRECISION NOT NULL,  -- side-adjusted
    mae_bps          DOUBLE PRECISION NOT NULL,  -- side-adjusted (<= 0)
    cost_bps         DOUBLE PRECISION,           -- the round trip the model was judged against
    net_bps          DOUBLE PRECISION,           -- NULL when no model cost was attached
    computed_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (candidate_id, horizon_minutes)
);
CREATE INDEX IF NOT EXISTS idx_cmo_computed ON altstrategy.candidate_market_outcomes (computed_at);

-- 3. trend_pullback is redefined as a setup (Bitget 1h regime + 15m pullback)
--    whose edge has not been shown out of sample: CANDIDATE, not ACTIVE. The
--    override row makes the status explicit instead of relying on the shipped
--    default, and the event records why.
INSERT INTO altstrategy.strategy_lifecycle (strategy_id, strategy_version, lifecycle_status)
VALUES ('trend_pullback', '1.0.0', 'CANDIDATE')
ON CONFLICT (strategy_id) DO UPDATE SET lifecycle_status = 'CANDIDATE', updated_at = now()
 WHERE altstrategy.strategy_lifecycle.lifecycle_status = 'ACTIVE';
INSERT INTO altstrategy.lifecycle_events (event_id, strategy_id, strategy_version, from_status,
                                          to_status, reason, sample_count, rollback, policy_version,
                                          created_at_ms)
SELECT 'lc-0009-trend_pullback', 'trend_pullback', '1.0.0', 'ACTIVE', 'CANDIDATE',
       'ml_first: redefined as 1h-regime + 15m-pullback setup; no out-of-sample edge', 0, false,
       'ml-first-1', (extract(epoch FROM now()) * 1000)::bigint
WHERE NOT EXISTS (SELECT 1 FROM altstrategy.lifecycle_events WHERE event_id = 'lc-0009-trend_pullback');
