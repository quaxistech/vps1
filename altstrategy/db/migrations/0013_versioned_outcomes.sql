-- Versioned market outcomes and learning evidence (QUALITY-04 / QUALITY-05).
-- Additive; no existing row is rewritten.
--
-- evaluator_version NULL is version 1: every outcome written before this
-- column existed (outcomes.hpp reconstruct_outcome). Version 2
-- (reconstruct_outcome_v2) resolves the same plan without version 1's
-- optimism -- gap-aware fills, trade-through limits, no exit credited inside
-- the entry bar unless the entry mechanics prove its order, no heuristic
-- ordering of two barriers inside one bar, gaps in the path made
-- DATA_INCOMPLETE -- and says which rule decided each outcome (resolution).
-- Version 2 also writes NULL, not 0, for the returns of an outcome that never
-- held a position (ENTRY_NOT_REACHED, ENTRY_EXPIRED, AMBIGUOUS): 5 779
-- ENTRY_EXPIRED rows written as a 0.0 return had entered the posteriors as
-- trades that broke even.
ALTER TABLE altstrategy.market_outcomes ADD COLUMN IF NOT EXISTS evaluator_version SMALLINT;
ALTER TABLE altstrategy.market_outcomes ADD COLUMN IF NOT EXISTS fill_model TEXT;
ALTER TABLE altstrategy.market_outcomes ADD COLUMN IF NOT EXISTS resolution TEXT;
ALTER TABLE altstrategy.market_outcomes ADD COLUMN IF NOT EXISTS path_bar_ms INTEGER;
ALTER TABLE altstrategy.market_outcomes ADD COLUMN IF NOT EXISTS path_gap_count INTEGER;

-- The minimum plan-geometry version of the outcomes a posterior row was
-- estimated from (learning.min_plan_geometry_version when it was written);
-- NULL is 1, i.e. it may include outcomes of the defective geometry. The
-- engine loads only rows at or above the configured minimum, so raising it
-- is the approved cutover to clean evidence and lowering it the rollback --
-- nothing is deleted either way.
ALTER TABLE altstrategy.strategy_posteriors ADD COLUMN IF NOT EXISTS evidence_version SMALLINT;
