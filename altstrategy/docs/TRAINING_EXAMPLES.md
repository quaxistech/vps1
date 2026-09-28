# Training Examples

The `SignalTrainingArchive` is a set of PostgreSQL tables + a periodic
materialization step in the maintenance scheduler, not a separate service.
When a proposal's outcome matures, an example is written to
`signal_training_examples` (+ `signal_training_labels`, `signal_training_context`).

## Example fields

`example_id`, `signal_proposal_id`, `candidate_id`, `plan_variant_id`,
`observation_time_ms`, `available_at_ms`, `label_matured_at_ms`,
`canonical_pair_id`, `strategy_id/version`, `parameter_set_id`, `side`,
`proposal_grade`, `feature_manifest_hash`, `label_spec_hash`, `quality`,
`context_id`, point-in-time `input_features`, plan geometry, market outcome,
`dataset_eligible`, and a monotone `sequence` (the incremental watermark).

## Labels

**Categorical**: `TARGET_FIRST`, `STOP_FIRST`, `TIME_STOP`, `ENTRY_NOT_REACHED`,
`ENTRY_EXPIRED`, `AMBIGUOUS`, `DATA_INCOMPLETE`.

**Continuous**: `net_return_after_costs`, `MFE`, `MAE`, `time_to_entry`,
`time_to_target`, `time_to_stop`, `holding_duration`.

`AMBIGUOUS` and `DATA_INCOMPLETE` set `binary_label_usable = false` and are never
mixed into the binary-label set. `INVALID` proposals are excluded entirely.

## Eligibility

`dataset_eligible = (binary_label_usable AND grade != INVALID)`. Only eligible,
matured examples are exported into training snapshots.
