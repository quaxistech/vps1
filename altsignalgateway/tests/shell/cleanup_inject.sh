#!/usr/bin/env bash
# Remove journal rows left by an adversarial-input run.
#
# The journal's DELETE trigger refuses anything that is not the retention pass,
# which is the point of it. This script uses the SAME marker the retention pass
# sets, deliberately and narrowly: only rows whose signal id carries the
# injector's prefix, which the pipeline can never produce.
#
# It exists because an early build journalled a test message before the size
# bound was added. A current build refuses those messages outright, so this
# should find nothing.
set -uo pipefail

VPS="${ALTSGW_HOST:?set ALTSGW_HOST=user@host}"
SSH_KEY="${ALTSIGNALGATEWAY_SSH_KEY:-$HOME/.ssh/altstrategy_vps_ed25519}"

# The deletion is recorded in retention_runs like any other, so a gap in the
# sequence can always be explained by an audited removal rather than looking
# like a lost allocation.
ssh -o BatchMode=yes -i "$SSH_KEY" "$VPS" "sudo -u postgres psql -d altspotter -c \"
BEGIN;
SET LOCAL altsignalgateway.retention_pass = 'on';
WITH ev AS (
    DELETE FROM altsignalgateway.delivery_events
     WHERE signal_id LIKE 'sqfin_inject_%' RETURNING 1),
     sig AS (
    DELETE FROM altsignalgateway.signal_journal
     WHERE signal_id LIKE 'sqfin_inject_%' RETURNING sequence)
INSERT INTO altsignalgateway.retention_runs (finished_at, rows_deleted, detail)
SELECT now(), (SELECT count(*) FROM ev) + (SELECT count(*) FROM sig),
       jsonb_build_object('reason', 'adversarial-input test cleanup',
                          'signal_journal', (SELECT count(*) FROM sig),
                          'delivery_events', (SELECT count(*) FROM ev),
                          'sequences', (SELECT coalesce(jsonb_agg(sequence), '[]'::jsonb) FROM sig));
COMMIT;\""
