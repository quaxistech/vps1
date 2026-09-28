-- The gap detector judged days the walk had never reached.
--
-- When METAL.XAUUSD/bid stopped at 2026-05-16 on a connection reset, the scan
-- looked at the whole requested window and filed every weekend beyond that
-- point as `market_closed` -- asserting the market was shut on days nobody had
-- asked about, from a calendar rather than from evidence. Thirty-four such
-- claims were written.
--
-- The detector now only judges ground behind a series' watermark and reports
-- anything beyond it as `not_walked`, which is a statement about our coverage
-- rather than about the market. This clears what the old reading recorded; the
-- next scan re-records what is true.

BEGIN;

DELETE FROM macro_series_gaps g
USING macro_ingest_cursors c
WHERE c.instrument_id = g.instrument_id
  AND c.provider_id   = g.provider_id
  AND c.timeframe     = g.timeframe
  AND g.gap_start    >= c.watermark;

COMMIT;
