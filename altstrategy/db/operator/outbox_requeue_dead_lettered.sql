-- Requeue dead-lettered outbox messages: the publisher gave up on them after
-- publication.max_age_hours, they kept their payload, and once the cause is
-- fixed (a stream that did not capture the subject, a long NATS outage) they
-- are sent again under their original Nats-Msg-Id.
--
-- Operator-applied, as altstrategy_app. Not a migration. A requeued row gets
-- one fresh attempt now; if that attempt fails too, the row is older than the
-- maximum age and is dead-lettered again at once -- fix the cause first.
--
--   psql -X -v ON_ERROR_STOP=1 -d altstrategy -f db/operator/outbox_requeue_dead_lettered.sql
--
-- To requeue only some rows, add a predicate on message_id, kind or
-- dead_lettered_at_ms to the WHERE clause.

UPDATE altstrategy.publication_outbox
   SET dead_lettered_at_ms = NULL,
       attempts = 0,
       next_attempt_at_ms = (extract(epoch FROM clock_timestamp()) * 1000)::bigint,
       last_error = 'requeued after: ' || COALESCE(last_error, 'unknown')
 WHERE dead_lettered_at_ms IS NOT NULL
   AND settled_at IS NULL;
