-- Distinguish a MEASURED market observation from the conservative stand-in used
-- when the book is unavailable.
--
-- Until now nothing downstream could tell them apart. `context_assembler` read
-- the spread with a defaulting lookup under a feature name no producer emits
-- (`spread_bps`; AltTech publishes `book_spread_bps`), so the column recorded
-- the fallback constant 10.0 as though it were an observed spread -- against a
-- real measured spread of 1.4-1.9 bps across four venues. A defaulting lookup
-- cannot report a name it never found, so the substitution was invisible in the
-- data it produced.
--
-- NULL is deliberate for existing rows: these flags were not recorded when they
-- were written, and a few historical contexts carry spreads that differ from the
-- fallback, so asserting `false` for all of history would be a claim this
-- migration cannot support. NULL means "not recorded", not "not observed".
ALTER TABLE altstrategy.decision_context_snapshots
    ADD COLUMN IF NOT EXISTS spread_observed BOOLEAN,
    ADD COLUMN IF NOT EXISTS depth_observed  BOOLEAN;

COMMENT ON COLUMN altstrategy.decision_context_snapshots.spread_observed IS
    'TRUE: spread_bps is a measurement from the order book at T0. '
    'FALSE: it is the conservative fallback, no book was available. '
    'NULL: predates the flag (see migration 0004).';
COMMENT ON COLUMN altstrategy.decision_context_snapshots.depth_observed IS
    'TRUE: top_depth_usdt is a measurement. FALSE: fallback. NULL: predates the flag.';
