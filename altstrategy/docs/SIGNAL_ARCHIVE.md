# Signal Archive

`signal_archive` is an **internal component**, not a separate service. It
persists the full decision lineage to the `altstrategy` schema as the engine
runs (`src/engine/io/decision_store.cpp`), preserving a complete, replayable
decision history for downstream signal delivery and execution consumers.

## What is archived

Every: `StrategyCandidate` (+ reasons/warnings), ensemble snapshot, ranking
snapshot (+ per-candidate components), plan variant, `TradeSignalProposal`
(+ warnings + state events), point-in-time `DecisionContext`, market outcome,
counterfactual outcome, execution feedback and attribution.

## Grades

`STRONG`, `NORMAL`, `WEAK`, `EXPERIMENTAL`, `SHADOW` proposals are all archived
**and** eligible for the training dataset. `INVALID` proposals are archived for
audit but flagged so they never enter the training set.

## Point-in-time correctness

The context snapshot stored with each proposal captures exactly what was
available at creation time (`observation_time`, `available_at`,
`context_revision`, `feature_manifest_hash`). It is never overwritten with later
recomputed values without a new `context_revision` (see
`docs/POINT_IN_TIME_CORRECTNESS.md`).

`decision_context_snapshots.snapshot` carries the NAMED analytics the strategies
actually decided on — the Bitget indicator summary, the microstructure window,
the cross-venue set, the reference-venue set and both regime views — as a JSONB
object. The scalar columns beside it describe the venue; this column describes
what the engine read. Only decoded, named values are stored, never a producer's
raw ordered vector: those are the quantities that have a meaning without the
producer's feature-set catalogue, and they keep the row near 2 KB against the
table's 14-day retention. Encoding lives in
`src/engine/io/include/altstrategy/engine_io/context_json.hpp` (tested by
`tests/unit/test_context_json.cpp`): non-finite values are omitted rather than
streamed as `nan` (which JSONB rejects, aborting the whole insert), and doubles
are written at full precision so a stored price is the price the decision was
taken on.

## Idempotency & restart recovery

Deterministic ids + `UNIQUE(natural_key)` make every archive write idempotent, so
a restart or NATS redelivery reproduces the same rows. Proposals awaiting
outcome are found by the maturation query (`proposals_awaiting_outcome`) and
resolved on the next scheduler pass — no in-flight state is lost across a
restart.
