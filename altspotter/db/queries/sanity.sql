-- Санити-проверки хранилища.
SELECT 'duplicate_1m_keys' AS check, count(*) AS value FROM (
    SELECT exchange_id, instrument_id, open_time
    FROM market_candles_1m GROUP BY 1, 2, 3 HAVING count(*) > 1) d
UNION ALL
SELECT 'stale_unclosed_1m', count(*) FROM market_candles_1m
 WHERE NOT is_closed AND open_time < now() - interval '10 minutes'
UNION ALL
SELECT 'revision_gt_20', count(*) FROM market_candles_1m WHERE revision > 20
UNION ALL
SELECT 'rejected_events', count(*) FROM rejected_market_events
UNION ALL
SELECT 'incomplete_agg', count(*) FROM market_candles_agg
 WHERE quality_status = 'incomplete';
