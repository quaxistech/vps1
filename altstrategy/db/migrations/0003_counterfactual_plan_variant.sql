-- 0003 — counterfactual_outcomes keyed per PLAN VARIANT.
--
-- Why: the ranking emits exactly one candidate per ranking_id (measured: 1.00
-- candidates/ranking over 162 rankings), so there are no rival candidates to
-- regret. The real, measurable alternative is the PLAN VARIANT: every candidate
-- produces exactly three (measured: 265 candidates, 3 variants each), each with
-- its own stop/target geometry and its own independently reconstructed outcome.
--
-- The original unique key (ranking_id, candidate_id) collapses those three into
-- one row, which is why the table could never hold a meaningful comparison.
-- Widening the key to include plan_version lets each variant be recorded against
-- the one that was chosen, turning "was this the best geometry we had?" into a
-- question the data can answer.

ALTER TABLE altstrategy.counterfactual_outcomes
    ADD COLUMN IF NOT EXISTS plan_version INTEGER NOT NULL DEFAULT 1;

ALTER TABLE altstrategy.counterfactual_outcomes
    DROP CONSTRAINT IF EXISTS counterfactual_outcomes_ranking_id_candidate_id_key;

DROP INDEX IF EXISTS altstrategy.counterfactual_outcomes_ranking_id_candidate_id_key;

CREATE UNIQUE INDEX IF NOT EXISTS counterfactual_outcomes_variant_key
    ON altstrategy.counterfactual_outcomes (ranking_id, candidate_id, plan_version);

-- Attribution is read back per proposal when auditing a losing cohort.
CREATE INDEX IF NOT EXISTS outcome_attribution_dominant_idx
    ON altstrategy.outcome_attribution (dominant_factor);
