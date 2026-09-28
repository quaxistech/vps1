-- Пропуски: открытые и в ремонте.
SELECT ei.exchange_symbol,
       hg.gap_start,
       hg.gap_end,
       hg.expected_rows,
       hg.status,
       hg.attempts,
       hg.next_attempt_at,
       hg.last_error
FROM history_gaps hg
JOIN exchange_instruments ei
  ON ei.exchange_id = hg.exchange_id AND ei.instrument_id = hg.instrument_id
WHERE hg.status IN ('open', 'repairing')
ORDER BY hg.gap_start;
