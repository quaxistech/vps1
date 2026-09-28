-- Repairing a gap is incremental: one command covers at most
-- `max_gap_minutes_per_command` minutes, so a multi-hour gap needs many
-- dispatches.  Without a cursor the dispatcher re-requested the FIRST chunk
-- every time, burned the attempt budget on data it had already recovered, and
-- then condemned the gap as unrepairable with the rest never requested at all.
--
-- The cursor records the oldest minute still missing at the last dispatch, so
-- progress can be distinguished from being stuck: attempts accumulate only
-- while the cursor stands still.
ALTER TABLE history_gaps ADD COLUMN IF NOT EXISTS repair_cursor timestamptz;

COMMENT ON COLUMN history_gaps.repair_cursor IS
    'Oldest still-missing minute at the last repair dispatch; NULL before the first dispatch.';
