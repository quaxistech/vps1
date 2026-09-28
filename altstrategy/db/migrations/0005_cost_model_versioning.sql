-- One cost definition, explicitly versioned, carried from the plan to the score.
--
-- Three different round-trip costs coexisted in this pipeline and disagreed
-- about the same trade:
--
--   * outcomes.hpp             8.0 bps  -- a struct default no code path ever
--                                          overrode, so EVERY realised outcome
--                                          in this table was scored at 8 bps
--                                          while its comment claimed to include
--                                          spread and slippage and included
--                                          neither;
--   * proposal.cpp / ranking  25.5 bps  -- 2x4 fees + a 10 bps FALLBACK spread
--                                          + 7.5 bps derived from it, because no
--                                          order book was being persisted;
--   * AltML training          16.0 bps  -- 12 fee + 4 slippage, from its config.
--
-- So a candidate was ranked against one cost, planned against it, and then
-- scored against another -- and the measured edge that gates execution came out
-- of the third. Versioning is what makes that impossible to do silently again.
--
--   version 1 = the legacy flat 8.0 bps. Recorded, not rewritten: it is what
--               was actually applied to every existing row, and back-dating a
--               newer definition onto old outcomes would fabricate history.
--   version 2 = decomposed and observed at T0 (see fee_bps_round_trip,
--               spread_cost_bps, slippage_cost_bps below).
--
-- Rows of different versions must never be pooled into one expectancy without
-- saying so; that is the whole reason the column exists.

ALTER TABLE altstrategy.trade_signal_proposals
    ADD COLUMN IF NOT EXISTS cost_model_version  INTEGER,
    ADD COLUMN IF NOT EXISTS fee_bps_round_trip  DOUBLE PRECISION,
    ADD COLUMN IF NOT EXISTS spread_cost_bps     DOUBLE PRECISION,
    ADD COLUMN IF NOT EXISTS slippage_cost_bps   DOUBLE PRECISION,
    ADD COLUMN IF NOT EXISTS round_trip_cost_bps DOUBLE PRECISION,
    ADD COLUMN IF NOT EXISTS spread_observed     BOOLEAN;

ALTER TABLE altstrategy.market_outcomes
    ADD COLUMN IF NOT EXISTS cost_model_version  INTEGER,
    ADD COLUMN IF NOT EXISTS round_trip_cost_bps DOUBLE PRECISION;

-- Existing outcomes: state plainly what was applied to them.
UPDATE altstrategy.market_outcomes
   SET cost_model_version = 1, round_trip_cost_bps = 8.0
 WHERE cost_model_version IS NULL;

COMMENT ON COLUMN altstrategy.market_outcomes.cost_model_version IS
    '1 = legacy flat 8.0 bps (fees only, despite the old comment). '
    '2 = decomposed fee + spread + slippage observed at T0. See migration 0005.';
COMMENT ON COLUMN altstrategy.trade_signal_proposals.spread_observed IS
    'TRUE: spread_cost_bps came from a real book reading at T0. '
    'FALSE: from the conservative fallback. NULL: predates migration 0005.';
