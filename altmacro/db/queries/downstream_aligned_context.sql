-- How a downstream service aligns altmacro's series to its own time grid.
--
-- This is the integration contract, written as SQL rather than as prose, and it
-- exists to make one rule impossible to get wrong: a series is available when it
-- became KNOWABLE, not when it was observed.
--
--   * An intraday market bar is knowable at its own event_time: the market
--     printed it then. The as-of join takes the last bar at or before the grid
--     point -- never the next one, which is the whole point.
--
--   * A daily reference value is knowable at its PUBLICATION, which is later
--     than the date it describes and sometimes by days: the Federal Reserve's
--     H.10 for a Friday is not public on that Friday. Where the source states
--     no release time, ingest_time is used instead -- an upper bound on when we
--     could have known, which is honest, whereas event_time would be a fiction
--     that hands a model tomorrow's news.
--
-- altmacro states what each number is. It does not say what any of them mean
-- together: no correlation, no beta, no lead-lag, no regime. That is AltTech's
-- and AltML's work, and this query deliberately stops short of it.
--
-- Parameters: :grid_start, :grid_end, :step (an interval, e.g. '15 minutes').

WITH grid AS (
    SELECT generate_series(:'grid_start'::timestamptz,
                           :'grid_end'::timestamptz,
                           :'step'::interval) AS t
),
intraday AS (
    SELECT g.t,
           i.code AS instrument,
           b.price_basis,
           b.close,
           b.event_time,
           -- How stale the aligned value is. A consumer that cares can reject
           -- anything older than it is willing to trust; without this it cannot
           -- tell a live quote from a Friday-evening one carried through a
           -- weekend.
           EXTRACT(EPOCH FROM (g.t - b.event_time))::bigint AS age_sec
    FROM grid g
    CROSS JOIN LATERAL (
        SELECT unnest(ARRAY['FX.USDCNH','METAL.XAUUSD','FX.EURUSD','FX.USDJPY']) AS code
    ) w
    JOIN macro_instruments i ON i.code = w.code
    LEFT JOIN LATERAL (
        SELECT b.close, b.event_time, b.price_basis
        FROM macro_price_bars b
        WHERE b.instrument_id = i.id
          AND b.price_basis = 'bid'
          -- VALID only, deliberately. Roughly a third of this archive is STALE:
          -- Dukascopy repeats the last price through weekends and quiet minutes
          -- at zero volume. Including those would hand back a fresh-looking
          -- quote with age_sec = 0 for a market that has been shut since Friday.
          -- Taking the last genuinely traded bar and reporting its age is the
          -- honest answer, and it lets the consumer decide what staleness it
          -- will tolerate.
          AND b.quality_status = 'VALID'
          AND b.event_time <= g.t
        ORDER BY b.event_time DESC
        LIMIT 1
    ) b ON true
),
reference AS (
    SELECT g.t,
           i.code AS instrument,
           v.value,
           v.event_time AS observed_for,
           COALESCE(v.publication_time, v.ingest_time) AS knowable_at
    FROM grid g
    CROSS JOIN LATERAL (
        SELECT unnest(ARRAY['REF.USDCNY.FED','REF.USDCNY.PBOC','IDX.VIX',
                            'RATE.US.10Y','RATE.US.10Y.REAL']) AS code
    ) w
    JOIN macro_instruments i ON i.code = w.code
    LEFT JOIN LATERAL (
        SELECT v.value, v.event_time, v.publication_time, v.ingest_time
        FROM macro_reference_values v
        WHERE v.instrument_id = i.id
          AND v.quality_status = 'VALID'
          -- Availability, not observation. Selecting on event_time here is
          -- exactly how look-ahead gets manufactured.
          AND COALESCE(v.publication_time, v.ingest_time) <= g.t
        ORDER BY COALESCE(v.publication_time, v.ingest_time) DESC, v.event_time DESC
        LIMIT 1
    ) v ON true
)
SELECT t, instrument, close AS value, age_sec, 'market' AS kind FROM intraday
UNION ALL
SELECT t, instrument, value, EXTRACT(EPOCH FROM (t - knowable_at))::bigint, 'reference' FROM reference
ORDER BY t, kind, instrument;
