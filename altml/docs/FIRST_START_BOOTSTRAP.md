# First start & bootstrap

On a fresh install there is no `ACTIVE` model. AltML starts up fully, stays
process-ready, and serves safely in `DETERMINISTIC_ONLY` mode (no prediction
is published) until it has trained and promoted its first model — it never
fabricates a prediction and never blocks AltSpotter/AltTech.

## What happens on first start

1. `altml run` provisions its JetStream topology (validates the externally
   owned input stream, creates its own output/control streams and consumer);
2. the label builder starts computing labels from AltSpotter's candle history
   and the inference component starts archiving AltTech's live feature
   vectors into `altml.feature_vectors` — both accumulate from real observed
   market data, nothing is backfilled or invented;
3. the trainer checks `training.min_rows`: while the joined feature+label
   history is below that threshold, it logs
   `insufficient labeled history` and defers, rechecking every
   `training.retry_minutes`;
4. once enough history exists (and the host is not under resource pressure —
   see below), the trainer runs its first walk-forward cycle and, if every
   [promotion gate](PROMOTION_GATES.md) passes, activates the first model.

## Readiness

systemd `READY=1` is sent once every component's `init()` has succeeded —
**an ACTIVE model is not required for process readiness.** Inference simply
serves in `DETERMINISTIC_ONLY` mode (visible in `inference_stats` journal
events and the aggregated systemd `STATUS` line) until a model exists.

## Resource-aware scheduling

The trainer defers a due cycle with `JOB_DEFERRED_RESOURCE_PRESSURE` when the
1-minute load average exceeds `training.max_load_average` or free memory is
below `training.min_free_mem_mib` — live inference always keeps priority over
training on a shared host. This is expected, safe behavior on a busy VPS, not
a failure; the cycle simply runs on the next check.

## Restart safety

Bootstrap has no separate state machine to resume: labels and archived
feature vectors are idempotent (`UPSERT`/`ON CONFLICT DO NOTHING` by natural
key), so an interrupted first start simply continues accumulating history on
the next start, and the trainer re-evaluates `min_rows` from what is actually
in the database — never from an in-memory counter that a restart could lose.
