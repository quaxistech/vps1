-- Свежесть данных: возраст последней закрытой минуты на инструмент.
SELECT ei.exchange_symbol,
       max(c.open_time)                                        AS latest_1m,
       round(extract(epoch FROM now() - max(c.open_time)))     AS age_sec,
       count(*) FILTER (WHERE c.open_time > now() - interval '1 hour') AS rows_last_hour
FROM exchange_instruments ei
JOIN market_candles_1m c
  ON c.exchange_id = ei.exchange_id AND c.instrument_id = ei.instrument_id
WHERE ei.is_whitelisted
GROUP BY ei.exchange_symbol
ORDER BY age_sec DESC;
