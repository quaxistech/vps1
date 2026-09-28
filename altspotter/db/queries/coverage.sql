-- Покрытие 1m-истории по инструментам whitelist.
SELECT ei.exchange_symbol,
       ei.status                                   AS instrument_status,
       hc.status                                   AS coverage_status,
       hc.actual_from,
       hc.covered_until,
       hc.row_count,
       hc.gap_count,
       hc.open_gap_count,
       hc.boundary_reached,
       hc.bootstrap_completed_at
FROM exchange_instruments ei
JOIN exchanges e ON e.id = ei.exchange_id AND e.code = 'bitget'
LEFT JOIN history_coverage hc
       ON hc.exchange_id = ei.exchange_id
      AND hc.instrument_id = ei.instrument_id
      AND hc.timeframe = '1m'
WHERE ei.is_whitelisted
ORDER BY ei.exchange_symbol;
