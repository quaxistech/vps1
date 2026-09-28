# Point-in-Time Correctness and the Leakage Audit

A model trained on the future looks excellent in every report and loses money in
production. Every claim this service makes about point-in-time correctness is
therefore checked mechanically, in three places: the unit tests, before every
training cycle, and in `altsignalquality validate`. A finding is **fatal** — the
cycle aborts and no model is registered.

## T0

`T0 = candidate.created_at_ms`, AltStrategy's own signal creation time. Every
feature must be knowable then. Two structural guarantees:

1. **The live vector is the training vector.** A signal scored live has its
   exact feature vector stored on the evaluation row; the training example built
   when its outcome matures reuses that vector byte for byte. There is no
   reconstruction step to drift.
2. **One assembler.** Backfilled rows — signals that predate this service — call
   the same `domain::assemble()` on a snapshot rebuilt from AltStrategy's
   immutable point-in-time archive.

## The hard case: as-of priors

The `prior_*` features answer "how has this strategy / pair / regime behaved so
far?". Reading them from today's aggregates would hand a June signal knowledge
of August outcomes. Two things prevent that:

**Live.** `outcome_priors` only ever folds in outcomes whose maturation time has
passed, against a durable watermark. A signal arriving now sees exactly what was
knowable now.

That equivalence — "everything matured so far" = "everything knowable at T0" —
holds only while the evaluation follows the signal promptly. A durable consumer
replaying a month of retained history evaluates *old* signals *today*, and their
priors would then include outcomes from after the signal. So a stored vector
counts as a LIVE training row only when
`evaluated_at_ms − signal_created_at_ms ≤ evaluator.max_signal_age_sec`;
anything slower is rebuilt by the backfill path, where the priors are genuinely
replayed as-of. Those signals are also rejected at decision time as
`SIGNAL_STALE`, so the same lag is visible in both halves of the system.

**Backfill.** `domain::sweep_as_of()` merges maturation events and read events
in time order and advances the accumulator to each signal's own T0 before
reading it. Ties resolve in favour of the maturation: an outcome that matured
*at* T0 was knowable at T0.

Both paths use the same `PriorAccumulator`, the same shrinkage and the same
decay, so the training features and the serving features are the same quantity.

## The hard case: the maturation purge in the splits

A signal's features exist at T0, but its label only exists when the outcome
matures — up to the hard 48h holding limit later. Splitting on observation time
alone therefore leaks: a training row observed before the validation window can
still carry a label that was only knowable inside it.

Every boundary is enforced twice:

```
train rows : observed before validation opens  AND  label matured before it opens (− embargo)
valid rows : observed inside validation        AND  label matured before TEST opens (− embargo)
test rows  : observed inside the test window   (untouched until the single final evaluation)
```

The validation purge matters as much as the training one: a threshold or a
calibration fitted on labels that matured after the test window opened would be
fitted on information from the test period.

The **embargo** (`training.embargo_minutes`, default 12h) sits on top, for the
residual autocorrelation between adjacent signals on the same instrument.

## What the audit checks

`src/altsignalquality/domain/leakage.hpp`:

| check | the failure it catches |
|---|---|
| contract availability | a feature added later whose source is only knowable after T0 |
| duplicate feature names | two positions indistinguishable in every report and importance ranking |
| label after observation | a row whose outcome matured at or before its own signal — physically impossible, so the join or the timestamp is wrong |
| non-positive time anchors | a missing or defaulted timestamp masquerading as a real one |
| duplicate signals | one signal in two slices, letting the model memorise a test row |
| fold ordering | train / validation / test out of chronological order |
| maturation purge | a training row whose *label* lands in the validation window — the failure ordinary time-series splitting misses entirely |
| refit ∩ test | a row in both the final refit and the held-out slice |

Each of these has a unit test that **plants the leak** and asserts the audit
reports it; a check that only passes on clean data is worthless.

## What is deliberately excluded

- Evaluation-time quantities (see [FEATURE_CONTRACT.md](FEATURE_CONTRACT.md)).
- `AMBIGUOUS` / `DATA_INCOMPLETE` outcomes, and outcomes where no position was
  taken (see [LABELS.md](LABELS.md)).
- Any recomputed context. AltStrategy stores its `DecisionContext` immutably and
  gives a recomputation a new `context_revision`; this service reads the row the
  signal actually referenced.
