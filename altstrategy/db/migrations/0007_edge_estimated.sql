-- Distinguish "the expected edge is zero" from "nobody estimated the edge".
--
-- Without an active AltML model the proposal builder used to fill the
-- forward-looking economics with invented constants:
--
--   p_entry_reached  = 0.99 (market) / 0.7 (limit)
--   p_timeout        = 0.1
--   p_ambiguous      = 0.05
--   p_target_before_stop = 1/(1+rr),  p_stop_before_target = rr/(1+rr)
--
-- The last two are fair odds, so p_target*target - p_stop*stop is
-- ALGEBRAICALLY ZERO for every reward/risk ratio. expected_gross_return was
-- therefore not computed from anything -- it was an identity in the shape of an
-- estimate -- and every proposal reported net = -cost as though measured.
--
-- The columns are KEPT, not dropped: historical rows are the record of what
-- those decisions were scored with, and deleting them erases that evidence.
-- They are no longer written; new rows carry NULL.

ALTER TABLE altstrategy.trade_signal_proposals
    ADD COLUMN IF NOT EXISTS edge_estimated BOOLEAN NOT NULL DEFAULT false;

COMMENT ON COLUMN altstrategy.trade_signal_proposals.edge_estimated IS
    'True when a model produced the forward-looking economics. When false, p_target_before_stop and expected_net_return are NULL: absent, not zero.';

COMMENT ON COLUMN altstrategy.trade_signal_proposals.p_entry_reached IS
    'DEPRECATED. Was a hardcoded 0.99/0.7, produced by nothing. No longer written.';
COMMENT ON COLUMN altstrategy.trade_signal_proposals.p_timeout IS
    'DEPRECATED. Was a hardcoded 0.1. No longer written.';
COMMENT ON COLUMN altstrategy.trade_signal_proposals.p_ambiguous IS
    'DEPRECATED. Was a hardcoded 0.05. No longer written.';
COMMENT ON COLUMN altstrategy.trade_signal_proposals.p_stop_before_target IS
    'DEPRECATED. Fair-odds complement of a fabricated probability. No longer written.';
COMMENT ON COLUMN altstrategy.trade_signal_proposals.expected_gross_return IS
    'DEPRECATED. Identically zero by construction whenever no model was active. No longer written.';
COMMENT ON COLUMN altstrategy.trade_signal_proposals.expected_utility IS
    'DEPRECATED. Was a copy of expected_net_return, not a utility. No longer written.';
