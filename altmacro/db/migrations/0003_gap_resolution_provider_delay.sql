-- A 404 from an archive means the file is not there. It says nothing about
-- whether the market traded, and the first bootstrap recorded it as though it
-- did: two days were filed as `market_closed` on a Thursday because Dukascopy
-- had not yet written that day's file when the walk reached it.
--
-- Measured on the live feed: a genuine weekend answers 200 with a full
-- 1440-record file whose quiet minutes are all-zero, whereas a day that has not
-- been archived yet answers 404. The two are now classified apart, and this
-- migration widens the vocabulary and repairs what was written under the old
-- reading.

BEGIN;

ALTER TABLE macro_series_gaps DROP CONSTRAINT IF EXISTS macro_series_gaps_resolution_check;
ALTER TABLE macro_series_gaps ADD CONSTRAINT macro_series_gaps_resolution_check
    CHECK (resolution IN ('backfilled', 'confirmed_absent_at_source', 'market_closed',
                          -- The source will have this period; it did not yet.
                          -- Deliberately left unresolved so it stays visible
                          -- until the live poller's lookback fills it.
                          'provider_delay', 'unresolved'));

-- Repair: any weekday filed as market_closed on the day the bootstrap ran was a
-- 404 against an archive that had not caught up, not a closure.
UPDATE macro_series_gaps
SET resolution = 'provider_delay', resolved_at = NULL
WHERE resolution = 'market_closed'
  AND EXTRACT(ISODOW FROM gap_start) BETWEEN 1 AND 5
  AND gap_start >= (SELECT min(started_at)::date FROM macro_ingest_runs WHERE kind = 'bootstrap');

COMMIT;

BEGIN;

-- Dukascopy does not write zeros for minutes with no ticks: it repeats the last
-- price at zero volume. The first decoder read those as traded observations, so
-- every weekend in the archive was stored as 1440 immaculate bars at a frozen
-- price -- roughly 29% of every FX series, and a market that appeared to deal
-- flat all weekend at no volume.
--
-- The rule was verified against the whole loaded window before being applied:
-- of 2.9 million bars, every zero-volume bar is flat and there is not one
-- counter-example, while minutes that genuinely traded once carry volume and
-- keep their VALID status.
--
-- The price is kept. It is the last known one and an as-of join needs it; what
-- changes is that it now says so.
UPDATE macro_price_bars
SET quality_status = 'STALE',
    quality_reason = 'no ticks at source in this minute; the feed carries the previous price forward',
    updated_at = now()
WHERE quality_status = 'VALID'
  AND volume = 0
  AND open = high AND high = low AND low = close;

COMMIT;

BEGIN;

-- Vintage support: implemented, verified, and deliberately not in the
-- production path -- because measurement says none of these series needs it.
--
-- ALFRED was called for real and honours the vintage: the value column comes
-- back named SERIES_YYYYMMDD and the loader refuses anything else, so a server
-- quietly serving today's numbers under a past date cannot pass. But comparing
-- the 2026-06-01 vintage against the current series found ZERO differing
-- observations: 11,846 common dates for DEXCHUS, 16,804 for DGS10, 9,500 for
-- VIXCLS, not one revision among them.
--
-- That is what these series are. An exchange rate and a yield are observed on
-- the day and do not change afterwards, unlike a statistic such as GDP that is
-- estimated and then revised. So production ingestion is latest-vintage only,
-- and this note records that it is a finding rather than an omission. A
-- revisable series added later -- CPI, payrolls, GDP -- would need an alfred
-- binding, and the machinery is already there for it.
UPDATE macro_providers
SET notes = notes || ' VERIFIED 2026-09-11: vintage retrieval works and the '
                  || 'returned column is checked, but no series in the current '
                  || 'working set is revised at all (0 differences across 38,150 '
                  || 'common observations vs the 2026-06-01 vintage), so nothing '
                  || 'is ingested vintage-aware. Production is latest-vintage only, '
                  || 'by measurement rather than by omission.'
WHERE code = 'alfred';

COMMIT;
