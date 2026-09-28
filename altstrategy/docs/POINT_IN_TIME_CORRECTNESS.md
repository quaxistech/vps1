# Point-in-Time Correctness

Training data must reflect only what was knowable at signal time. AltStrategy
enforces this end to end.

## Forbidden

- Using a future regime, future structure or future ML forecast.
- Overwriting a stored context with later recomputed values **without** a new
  `context_revision`.

## Mechanisms

- The `DecisionContext` is assembled from components with explicit
  revision/availability semantics and stored **immutably** in
  `decision_context_snapshots`. A new computation gets a new `context_revision`;
  the old snapshot is retained.
- Every proposal, candidate and training example references the exact
  `context_id` + `context_revision` used, plus the `feature_manifest_hash` and
  `label_spec_hash`.
- Required time fields are always present: `observation_time` (event time),
  `available_at`, `created_at`, `label_matured_at`, `context_revision`.
- Market outcomes are computed only **after** the full maturation window (entry
  validity + max holding / time-stop) has elapsed — never peeking ahead, and
  never waiting on execution feedback.
- `AMBIGUOUS` / `DATA_INCOMPLETE` outcomes are recorded honestly and excluded
  from the binary-label training set rather than being guessed into a win/loss.

## Result

Every training example is reconstructable to the exact information state at
signal time; there is no look-ahead leakage between a signal's context and its
label.
