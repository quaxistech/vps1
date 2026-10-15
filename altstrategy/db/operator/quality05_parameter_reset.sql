-- QUALITY-05: return every calibrated strategy to its compiled-default parameters.
--
-- Operator-applied, once, ONLY with explicit approval. Not a migration: the
-- migrator never runs it.
--
-- Every calibrated set in force on 2026-10-15 (15 of 16 strategies, most at
-- the bounds of their parameter schema) was fitted on outcomes of plan
-- geometry 1 -- the target rounded onto the entry (QUALITY-01) from the long
-- side's excursion for both sides (QUALITY-02) -- scored by outcome
-- evaluator 1. Those sets are kept, as 'rolled_back'; nothing is deleted.
--
-- The replacement is built from strategy_parameter_schemas, which the service
-- rewrites from its compiled catalogue at every start -- not from the seeded
-- 'ps_<strategy>_default' rows: two of them predate a parameter. Re-activating
-- a seeded row is not an option either way, and neither is leaving a strategy
-- with no active set: the start-up seed would then insert 'ps_<strategy>_default'
-- again and fail on the existing (retired) row.
--
-- The engine re-reads parameter sets with its adaptive state (every 5 min).
-- Pair with calibration.enabled: false until outcomes of geometry 2 have
-- matured, and learning.min_plan_geometry_version: 2.
--
-- Undo (same approval): set the 'ps_<strategy>_quality05_reset' rows to
-- 'retired' and the sets named in their derived_from back to 'active', in one
-- transaction.

BEGIN;
SET LOCAL lock_timeout = '5s';

CREATE TEMP TABLE quality05_reset ON COMMIT DROP AS
SELECT parameter_set_id AS rolled_back_id, strategy_id, strategy_version
  FROM altstrategy.strategy_parameter_sets
 WHERE status = 'active'
   AND parameter_set_id NOT IN ('ps_' || strategy_id || '_default', 'ps_' || strategy_id || '_quality05_reset');

UPDATE altstrategy.strategy_parameter_sets ps
   SET status = 'rolled_back'
  FROM quality05_reset r
 WHERE ps.parameter_set_id = r.rolled_back_id;

INSERT INTO altstrategy.strategy_parameter_sets
       (parameter_set_id, strategy_id, strategy_version, param_values, status, derived_from, activated_at)
SELECT 'ps_' || r.strategy_id || '_quality05_reset', r.strategy_id, r.strategy_version,
       jsonb_object_agg(s.param_name, s.default_value), 'active', r.rolled_back_id, now()
  FROM quality05_reset r
  JOIN altstrategy.strategy_parameter_schemas s
    ON s.strategy_id = r.strategy_id AND s.strategy_version = r.strategy_version
 GROUP BY r.strategy_id, r.strategy_version, r.rolled_back_id;

-- Every strategy rolled back has its default set, and none has two: or nothing commits.
DO $$
DECLARE
    rolled bigint;
    installed bigint;
BEGIN
    SELECT count(*) INTO rolled FROM quality05_reset;
    SELECT count(*) INTO installed
      FROM altstrategy.strategy_parameter_sets ps JOIN quality05_reset r
        ON ps.parameter_set_id = 'ps_' || r.strategy_id || '_quality05_reset'
     WHERE ps.status = 'active';
    IF installed <> rolled THEN
        RAISE EXCEPTION 'quality05 reset: % sets rolled back, % defaults installed', rolled, installed;
    END IF;
    IF EXISTS (SELECT 1 FROM altstrategy.strategy_parameter_sets WHERE status = 'active'
                GROUP BY strategy_id HAVING count(*) > 1) THEN
        RAISE EXCEPTION 'quality05 reset: a strategy would have more than one active set';
    END IF;
    RAISE NOTICE 'quality05 reset: % calibrated sets rolled back to compiled defaults', rolled;
END $$;

COMMIT;
