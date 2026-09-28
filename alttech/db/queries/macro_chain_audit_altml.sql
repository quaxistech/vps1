-- Second half of the macro chain audit, run against altml.
--   sudo -u postgres psql -d altml -f macro_chain_audit_altml.sql
\pset border 2
\echo '== 8. macro rows in the AltML archive start where crypto features start =='
SELECT (SELECT min(bar_open_time)::date FROM altml.feature_vectors WHERE exchange_id=1 AND feature_set_name='talib-core') AS crypto_from,
       (SELECT min(bar_open_time)::date FROM altml.feature_vectors WHERE exchange_id=0 AND instrument_id=0) AS macro_from,
       (SELECT count(*) FROM altml.feature_vectors WHERE exchange_id=0 AND instrument_id=0) AS macro_rows,
       (SELECT max(bar_open_time)::timestamp(0) FROM altml.feature_vectors WHERE exchange_id=0 AND instrument_id=0) AS macro_newest;

\echo '== 9. no orphan macro row before the first crypto feature =='
SELECT count(*) AS orphans FROM altml.feature_vectors m
WHERE m.exchange_id=0 AND m.instrument_id=0
  AND m.bar_open_time < (SELECT min(bar_open_time) FROM altml.feature_vectors WHERE exchange_id=1 AND feature_set_name='talib-core');

\echo '== 10. the archived block is byte-identical to AltTech''s (last 100 rows) =='
-- The FDW is mapped for altml_app only; postgres has no mapping and the
-- foreign table cannot be read as superuser.
SET ROLE altml_app;
SELECT count(*) AS compared,
       count(*) FILTER (WHERE a.values = t.values AND a.validity_bitmap = t.validity_bitmap) AS identical
FROM (SELECT bar_open_time, values, validity_bitmap FROM altml.feature_vectors
      WHERE exchange_id=0 AND instrument_id=0 ORDER BY bar_open_time DESC LIMIT 100) a
JOIN altml.fdw_macro_feature_vectors t ON t.bar_open_time=a.bar_open_time AND t.timeframe='1m';
RESET ROLE;

\echo '== 11. no model was promoted with the macro block =='
SELECT count(*) AS active_models_with_macro
FROM altml.models m JOIN altml.model_feature_compatibility c ON c.model_id=m.model_id
WHERE m.status='ACTIVE' AND c.aligned_sources::text LIKE '%macro%';
