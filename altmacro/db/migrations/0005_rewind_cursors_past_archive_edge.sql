-- The watermark stepped over a period the source had not archived yet.
--
-- The first full load ran on 2026-09-10 and asked for that day. The archive had
-- not been written, so the feed answered 404; the loader recorded it correctly
-- as `provider_delay` -- and then advanced the cursor past it. The resume
-- started after the hole, so three series are missing 00:00-21:16 of that day
-- while their watermark says the whole window is collected. A watermark that
-- claims ground nothing will ever revisit is the most dangerous kind of false
-- coverage, because no later run can notice.
--
-- The loader now stops at the archive's edge instead of stepping over it. This
-- rewinds any cursor that sits beyond an unfilled `provider_delay` gap so the
-- next run collects what was skipped.

BEGIN;

UPDATE macro_ingest_cursors c
SET watermark = g.gap_start,
    updated_at = now()
FROM (
    SELECT instrument_id, provider_id, timeframe, min(gap_start) AS gap_start
    FROM macro_series_gaps
    WHERE resolution = 'provider_delay' AND resolved_at IS NULL
    GROUP BY 1,2,3
) g
WHERE c.instrument_id = g.instrument_id
  AND c.provider_id   = g.provider_id
  AND c.timeframe     = g.timeframe
  AND c.watermark     > g.gap_start;

-- The same day is also recorded as an `unresolved` partial for the series that
-- got only its tail. Clearing those lets the next scan state what is true after
-- the refill rather than carrying the old reading forward.
DELETE FROM macro_series_gaps
WHERE resolution = 'unresolved'
  AND gap_start >= (SELECT min(gap_start) FROM macro_series_gaps
                    WHERE resolution = 'provider_delay');

COMMIT;
