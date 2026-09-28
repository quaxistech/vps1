-- Market outcomes are per PROPOSAL, not per candidate.
--
-- One candidate produces three trade-plan variants (AGGRESSIVE / BALANCED /
-- CONSERVATIVE), each published as its own proposal with its own entry, stop and
-- target. Those three geometries resolve differently against the same price
-- path — that is the entire point of generating them — so they have three
-- distinct outcomes.
--
-- The original UNIQUE (candidate_id, is_counterfactual) allowed only one, so:
--   * the second and third proposal of every candidate could never store their
--     outcome (INSERT ... ON CONFLICT DO NOTHING silently dropped it), and
--   * because the maturation query selects proposals that have no outcome row
--     linked to their own signal_proposal_id, those proposals stayed pending
--     forever and re-emitted an OUTCOME_FINAL state event on every maintenance
--     pass — 1.2 M events for 48 k proposals on the test deployment.
--
-- The natural key is therefore the proposal for a realised outcome, and the
-- candidate for a counterfactual (which has no proposal by definition). Two
-- partial unique indexes state exactly that; a plain multi-column UNIQUE cannot,
-- because NULL signal_proposal_id values do not conflict with one another.

ALTER TABLE altstrategy.market_outcomes
    DROP CONSTRAINT IF EXISTS market_outcomes_candidate_id_is_counterfactual_key;

-- A realised outcome belongs to exactly one proposal.
CREATE UNIQUE INDEX IF NOT EXISTS market_outcomes_proposal_key
    ON altstrategy.market_outcomes (signal_proposal_id)
    WHERE is_counterfactual = false;

-- A counterfactual has no proposal; it is keyed by the candidate it evaluates.
CREATE UNIQUE INDEX IF NOT EXISTS market_outcomes_counterfactual_key
    ON altstrategy.market_outcomes (candidate_id)
    WHERE is_counterfactual = true;

-- A realised outcome without a proposal id cannot be attributed to anything.
ALTER TABLE altstrategy.market_outcomes
    DROP CONSTRAINT IF EXISTS market_outcomes_realised_has_proposal;
ALTER TABLE altstrategy.market_outcomes
    ADD CONSTRAINT market_outcomes_realised_has_proposal
    CHECK (is_counterfactual OR signal_proposal_id IS NOT NULL);
