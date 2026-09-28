-- One row per series, stating what it is rather than what it is worth.
--
-- This is the answer to "what am I actually looking at": the authority behind
-- the number, its native cadence, the timezone its timestamps live in, whether
-- it is a market price or a published reference, whether it can be revised, and
-- how much of the target window is genuinely covered.
--
-- The CNY family is the reason this report exists in this form. USD/CNH,
-- the Federal Reserve's H.10 USD/CNY and the PBOC's central parity are three
-- separate rows here and three separate rows everywhere else, because on a
-- single date they read 6.7096, 6.7108 and 6.7766. Anything that presents them
-- as one series is presenting a policy spread as measurement noise.

SELECT
    i.code                                   AS instrument,
    i.series_class,
    p.code                                   AS provider,
    s.native_symbol,
    s.price_basis,
    i.native_frequency                       AS frequency,
    s.source_timezone                        AS timezone,
    s.timestamp_semantics,
    i.denomination,
    i.session_calendar,
    CASE WHEN pr.capabilities_vintages THEN 'vintage-aware' ELSE 'latest only' END
                                             AS revision_semantics,
    cov.earliest,
    cov.latest,
    cov.rows,
    cov.valid,
    cov.stale,
    cov.market_closed,
    cov.missing
FROM macro_instrument_sources s
JOIN macro_instruments i ON i.id = s.instrument_id
JOIN macro_providers   p ON p.id = s.provider_id
-- ALFRED is the only source in this registry that can answer "as known on
-- date D"; everything else serves the current vintage and says so.
LEFT JOIN LATERAL (SELECT p.code = 'alfred' AS capabilities_vintages) pr ON true
LEFT JOIN LATERAL (
    SELECT min(x.t) AS earliest, max(x.t) AS latest, count(*) AS rows,
           count(*) FILTER (WHERE x.q = 'VALID')         AS valid,
           count(*) FILTER (WHERE x.q = 'STALE')         AS stale,
           count(*) FILTER (WHERE x.q = 'MARKET_CLOSED') AS market_closed,
           count(*) FILTER (WHERE x.q = 'MISSING')       AS missing
    FROM (
        SELECT b.event_time AS t, b.quality_status AS q
        FROM macro_price_bars b
        WHERE b.instrument_id = s.instrument_id
          AND b.provider_id   = s.provider_id
          AND b.price_basis   = s.price_basis
        UNION ALL
        SELECT v.event_time, v.quality_status
        FROM macro_reference_values v
        WHERE v.instrument_id = s.instrument_id
          AND v.provider_id   = s.provider_id
    ) x
) cov ON true
ORDER BY i.series_class, i.code, s.price_basis;
