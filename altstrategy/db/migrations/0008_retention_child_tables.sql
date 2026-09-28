-- Retention policies for two CHILD tables named a column those tables do not
-- have, so the daily sweep raised `retention_table_failed` for each of them on
-- every run since deployment -- and the event carried no reason, so the failure
-- said nothing about itself.
--
-- Neither policy was ever needed. `strategy_candidate_reasons` and
-- `ranking_components` are keyed to their parents by FOREIGN KEY ... ON DELETE
-- CASCADE, so they are already removed with the parent row that owns them. A
-- policy on a child of a cascading parent is not just unusable here, it is a
-- second, competing definition of the same lifetime.
DELETE FROM altstrategy.retention_policies
 WHERE table_name IN ('strategy_candidate_reasons', 'ranking_components');
