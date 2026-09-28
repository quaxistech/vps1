-- Macro chain audit: AltMacro raw -> AltTech macro block -> AltML archive.
--
-- Run against the alttech database as postgres; the AltML side is reached by
-- the FDW in the reverse direction only, so its checks live in the second
-- block and are run against altml. Every check is a query over live rows.
--
--   sudo -u postgres psql -d alttech -f macro_chain_audit.sql

\pset border 2
\echo '== 1. raw is present and current (altmacro_ref) =='
SELECT i.code, count(*) AS rows, max(b.event_time)::timestamp(0) AS newest
FROM altmacro_ref.macro_price_bars b JOIN altmacro_ref.macro_instruments i ON i.id=b.instrument_id
WHERE b.event_time >= now() - interval '1 day' AND b.price_basis='bid' GROUP BY 1 ORDER BY 1;

\echo '== 2. the macro block is registered once, frozen, with the built fingerprint =='
SELECT name, version, scope, frozen, left(schema_hash,12) AS hash,
       (SELECT count(*) FROM alttech.feature_set_members m WHERE m.feature_set_version_id=v.id) AS members
FROM alttech.feature_set_versions v WHERE name='macro-core';

\echo '== 3. coverage and status =='
SELECT status, count(*) AS vectors, min(bar_open_time)::date AS earliest, max(bar_open_time)::timestamp(0) AS newest
FROM alttech.macro_feature_vectors GROUP BY 1 ORDER BY 2 DESC;

\echo '== 4. no position is permanently dead (last 2 days) =='
WITH names AS (
  SELECT m.position, m.output_name FROM alttech.feature_set_members m
  JOIN alttech.feature_set_versions v ON v.id=m.feature_set_version_id
  WHERE v.name='macro-core' AND v.version=1),
recent AS (SELECT validity_bitmap FROM alttech.macro_feature_vectors
           WHERE bar_open_time >= now() - interval '2 days')
SELECT n.output_name, round(100.0*avg(get_bit(r.validity_bitmap,n.position)),1) AS pct_valid
FROM names n CROSS JOIN recent r GROUP BY 1
HAVING avg(get_bit(r.validity_bitmap,n.position)) < 0.5 ORDER BY 2;

\echo '== 5. no invalid position carries a non-zero value (the bit is the truth) =='
WITH names AS (
  SELECT m.position FROM alttech.feature_set_members m
  JOIN alttech.feature_set_versions v ON v.id=m.feature_set_version_id
  WHERE v.name='macro-core' AND v.version=1)
SELECT count(*) AS violations
FROM alttech.macro_feature_vectors mv CROSS JOIN names n
WHERE mv.bar_open_time >= now() - interval '2 days'
  AND get_bit(mv.validity_bitmap, n.position) = 0 AND mv.values[n.position+1] <> 0;

\echo '== 6. the three CNY series are never equal in the block (bps gaps are non-zero) =='
WITH p AS (
  SELECT m.output_name, m.position FROM alttech.feature_set_members m
  JOIN alttech.feature_set_versions v ON v.id=m.feature_set_version_id
  WHERE v.name='macro-core' AND m.output_name IN ('cnh_vs_fixing_bps','cnh_vs_h10_bps','fixing_vs_h10_bps'))
SELECT p.output_name,
       count(*) FILTER (WHERE get_bit(mv.validity_bitmap,p.position)=1) AS valid_rows,
       count(*) FILTER (WHERE get_bit(mv.validity_bitmap,p.position)=1 AND mv.values[p.position+1]=0) AS exactly_zero,
       round(avg(mv.values[p.position+1]) FILTER (WHERE get_bit(mv.validity_bitmap,p.position)=1)::numeric,2) AS mean_bps
FROM alttech.macro_feature_vectors mv CROSS JOIN p
WHERE mv.bar_open_time >= now() - interval '7 days' GROUP BY 1 ORDER BY 1;

\echo '== 7. as-of: at 07:30 UTC the fix in use is YESTERDAY''s, at 08:30 it is TODAY''s (admitted 08:00) =='
-- The parity is set at 01:15 UTC but reaches AltMacro through an aggregator
-- hours later; the block admits it from 08:00. Both sides of that edge are
-- recomputed here from raw: a row at 07:30 must match yesterday's fix and a
-- row at 08:30 today's. Weekend rows are skipped (no parity is set).
WITH pos AS (SELECT m.position FROM alttech.feature_set_members m
  JOIN alttech.feature_set_versions v ON v.id=m.feature_set_version_id
  WHERE v.name='macro-core' AND m.output_name='cnh_vs_fixing_bps'),
s AS (
  SELECT mv.bar_open_time AS t, mv.values[(SELECT position FROM pos)+1] AS stored,
         EXTRACT(HOUR FROM mv.bar_open_time) < 8 AS before_admission
  FROM alttech.macro_feature_vectors mv
  WHERE mv.bar_open_time >= now() - interval '5 days'
    AND EXTRACT(HOUR FROM mv.bar_open_time) IN (7, 8) AND EXTRACT(MINUTE FROM mv.bar_open_time)=30
    AND EXTRACT(ISODOW FROM mv.bar_open_time) <= 5
    AND get_bit(mv.validity_bitmap,(SELECT position FROM pos))=1),
mid AS (
  SELECT s.t, (b.close+a.close)/2 AS cnh_mid
  FROM s
  JOIN altmacro_ref.macro_instruments i ON i.code='FX.USDCNH'
  JOIN altmacro_ref.macro_price_bars b ON b.instrument_id=i.id AND b.price_basis='bid' AND b.timeframe='1m' AND b.event_time=s.t
  JOIN altmacro_ref.macro_price_bars a ON a.instrument_id=i.id AND a.price_basis='ask' AND a.timeframe='1m' AND a.event_time=s.t)
SELECT to_char(s.t,'MM-DD HH24:MI') AS t,
  CASE WHEN s.before_admission THEN 'yesterday' ELSE 'today' END AS expected_fix,
  round(s.stored::numeric,2) AS stored,
  round(((mid.cnh_mid - y.value) / y.value * 10000)::numeric, 2) AS recomputed,
  CASE WHEN abs(s.stored - ((mid.cnh_mid - y.value) / y.value * 10000)) < 0.5 THEN 'ok' ELSE 'MISMATCH' END AS verdict
FROM s
JOIN mid ON mid.t = s.t
JOIN LATERAL (SELECT v.value FROM altmacro_ref.macro_reference_values v
              JOIN altmacro_ref.macro_instruments i ON i.id=v.instrument_id
              WHERE i.code='REF.USDCNY.PBOC' AND v.quality_status='VALID'
                AND date_trunc('day',v.event_time) <= date_trunc('day',s.t) - CASE WHEN s.before_admission THEN interval '1 day' ELSE interval '0' END
              ORDER BY v.event_time DESC LIMIT 1) y ON true
ORDER BY s.t;

\echo '== 7b. as-of: rates_age_hours at 12:00 UTC equals the FRED next-business-day rule recomputed from raw =='
-- FRED carries a Treasury close the next business day; the block admits it
-- from 00:00 UTC of the day after that (Mon-Thu close: +2 days, Fri: +4).
-- The rule is restated here in SQL on purpose: an audit that called the
-- service's own function would only prove the service agrees with itself.
WITH pos AS (SELECT m.position FROM alttech.feature_set_members m
  JOIN alttech.feature_set_versions v ON v.id=m.feature_set_version_id
  WHERE v.name='macro-core' AND m.output_name='rates_age_hours'),
s AS (
  SELECT mv.bar_open_time AS t, mv.values[(SELECT position FROM pos)+1] AS stored
  FROM alttech.macro_feature_vectors mv
  WHERE mv.bar_open_time >= now() - interval '14 days'
    AND EXTRACT(HOUR FROM mv.bar_open_time)=12 AND EXTRACT(MINUTE FROM mv.bar_open_time)=0
    AND get_bit(mv.validity_bitmap,(SELECT position FROM pos))=1),
obs AS (
  SELECT date_trunc('day', v.event_time) AS d,
         date_trunc('day', v.event_time) + CASE EXTRACT(ISODOW FROM v.event_time)
             WHEN 5 THEN interval '4 days' WHEN 6 THEN interval '3 days' ELSE interval '2 days' END AS usable_at
  FROM altmacro_ref.macro_reference_values v
  JOIN altmacro_ref.macro_instruments i ON i.id=v.instrument_id
  WHERE i.code='RATE.US.10Y' AND v.quality_status='VALID' AND v.value IS NOT NULL)
SELECT to_char(s.t,'MM-DD Dy') AS t, round(s.stored::numeric,1) AS stored_h,
       round(EXTRACT(EPOCH FROM (s.t - u.usable_at))/3600.0, 1) AS recomputed_h,
       CASE WHEN abs(s.stored - EXTRACT(EPOCH FROM (s.t - u.usable_at))/3600.0) < 0.1 THEN 'ok' ELSE 'MISMATCH' END AS verdict
FROM s
JOIN LATERAL (SELECT usable_at FROM obs WHERE obs.usable_at <= s.t ORDER BY usable_at DESC LIMIT 1) u ON true
ORDER BY s.t;

\echo '== 7c. as-of: the intraday side at minute t is the newest bar of the last hour that had landed (cutoff = hour(t - 30min)) =='
-- Dukascopy is hourly files; the block admits an hour thirty minutes after
-- it closes. The stored USDCNH spread at t must therefore equal the raw
-- spread of the bar at cutoff(t) - 1 minute, not of the bar at t.
WITH pos AS (SELECT m.position FROM alttech.feature_set_members m
  JOIN alttech.feature_set_versions v ON v.id=m.feature_set_version_id
  WHERE v.name='macro-core' AND m.output_name='cnh_spread_bps'),
s AS (
  SELECT mv.bar_open_time AS t, mv.values[(SELECT position FROM pos)+1] AS stored,
         date_trunc('hour', mv.bar_open_time - interval '30 minutes') - interval '1 minute' AS tick
  FROM alttech.macro_feature_vectors mv
  WHERE mv.bar_open_time >= now() - interval '7 days'
    AND EXTRACT(MINUTE FROM mv.bar_open_time) IN (0, 29, 30)
    AND EXTRACT(HOUR FROM mv.bar_open_time) IN (9, 14)
    AND get_bit(mv.validity_bitmap,(SELECT position FROM pos))=1),
raw AS (
  SELECT s.t, s.tick, s.stored,
         (a.close - b.close) / ((a.close + b.close) / 2) * 10000 AS raw_spread_at_tick
  FROM s
  JOIN altmacro_ref.macro_instruments i ON i.code='FX.USDCNH'
  JOIN altmacro_ref.macro_price_bars b ON b.instrument_id=i.id AND b.price_basis='bid' AND b.timeframe='1m' AND b.event_time=s.tick
  JOIN altmacro_ref.macro_price_bars a ON a.instrument_id=i.id AND a.price_basis='ask' AND a.timeframe='1m' AND a.event_time=s.tick)
SELECT to_char(t,'MM-DD HH24:MI') AS t, to_char(tick,'HH24:MI') AS bar_used, round(stored::numeric,3) AS stored,
       round(raw_spread_at_tick::numeric,3) AS recomputed,
       CASE WHEN abs(stored - raw_spread_at_tick) < 0.01 THEN 'ok' ELSE 'MISMATCH' END AS verdict
FROM raw ORDER BY t;
