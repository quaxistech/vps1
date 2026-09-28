-- Replace the ML uncertainty/OOD columns with what AltML actually produces.
--
-- `ml_uncertainty` was fed AltML's `confidence` -- |p_up - 0.5| * 2, where HIGH
-- means FAR from a coin flip -- while the ranking policy weighted it at -0.6.
-- The score penalised the model's most confident calls. `ml_ood` was never set
-- by anything, so its stored 0 asserted "in distribution" on every row.
--
-- The old columns are kept, not dropped: they hold historical rows whose values
-- were written under the old (wrong) meaning, and deleting them would erase the
-- evidence of what those decisions were scored with. They are no longer written.

ALTER TABLE altstrategy.trade_signal_proposals
    ADD COLUMN IF NOT EXISTS ml_conviction DOUBLE PRECISION;

COMMENT ON COLUMN altstrategy.trade_signal_proposals.ml_conviction IS
    'AltML confidence: |p_up - 0.5| * 2. Distance from a coin flip, NOT an uncertainty estimate.';
COMMENT ON COLUMN altstrategy.trade_signal_proposals.ml_uncertainty IS
    'DEPRECATED. Historically carried AltML conviction under an uncertainty name. No longer written.';
COMMENT ON COLUMN altstrategy.trade_signal_proposals.ml_ood IS
    'DEPRECATED. Never produced by any model; stored zeros are not measurements. No longer written.';
