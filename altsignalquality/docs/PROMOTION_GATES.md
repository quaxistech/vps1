# Promotion Gates

A training cycle only ever adds a **CANDIDATE** model. When optional training is
enabled, promotion requires every gate below to pass on the held-out slice the
model was never fitted on. A candidate that fails is registered **REJECTED**
with its gate results. Training is disabled by default. A promoted model's score
is diagnostic and does not gate signal publication.

## The population a gate is measured on

The trainer uses the eligible population selected by its configuration. By
default, that is the full eligible population
(`evaluate_on_recommended_only = false`). Its thresholds and uplift reports
describe a hypothetical score-based selection for model evaluation; no such
threshold is applied by the live evaluator. The dataset contains only proposal
geometries published on `signal.proposed.>`.

## Why not accuracy

Accuracy alone does not measure whether a score ranks outcomes usefully. The
training report therefore compares hypothetical score-based selection against
the full eligible candidate baseline.

Every gate is measured against the **baseline of taking every eligible
AltStrategy candidate in the same slice**. The headline objective is **return
per candidate offered** — total net return for the selected set divided by all
candidates in the slice. These metrics characterize a diagnostic model; the
live policy forwards or rejects signals by validity rules, not score threshold.

## The gates

| gate | config key | why |
|---|---|---|
| mean walk-forward ROC-AUC ≥ 0.52 | `min_mean_fold_auc` | the model must rank outcomes at all, across folds and not just at the end |
| ≥ 2 of 3 folds improve return per candidate | `min_positive_folds` | one lucky fold is not evidence |
| held-out return-per-candidate uplift ≥ 0 | `min_return_per_candidate_uplift` | **the gate that matters**: the desk must earn more per opportunity than it would have without the filter |
| held-out win-rate uplift ≥ 0 | `min_win_rate_uplift` | the accepted set must be cleaner, not merely smaller |
| accepted expectancy > 0 | — | a filter whose accepted set loses money is worse than useless |
| accepted count ≥ 30 | `min_accepted` | no promotion on a handful of signals |
| acceptance rate ∈ [0.15, 0.95] | `min/max_acceptance_rate` | a filter that takes 2% has no business; one that takes 99% is not a filter |
| ECE ≤ 0.15 | `max_ece` | a reported diagnostic `quality_score` must be calibrated |
| ≥ 60% of buckets non-negative in the weakest view | `min_stability_share` | a filter carried by one symbol, one strategy, one regime or one month is an accident |
| beats the incumbent on the shared holdout | — | champion–challenger, same applicable slice, each with its own calibration and threshold |
| uplift's **lower confidence bound** ≥ 0 | `min_uplift_lower_bound` | a point estimate is not a result; see below |
| positive rate ∈ [0.05, 0.95] | `min/max_positive_rate` | an effectively single-class dataset cannot train a filter, and its metrics mislead |
| ≥ 30% of declared features carry information | `min_informative_feature_share` | the 109-feature contract must contain usable input variation |

## Stability

Five views: **strategy**, **symbol**, **regime**, **month** and **side**.

Direction is its own view. Each bucket with at
least `stability_min_bucket_samples` signals is scored for return-per-candidate
uplift; the gate reads the *worst* view's share of non-negative buckets. A view
with fewer than two populated buckets says nothing, so it can neither fail nor
pass the gate.

## Statistical uncertainty

The uplift gate reads the **lower bound of a confidence interval**, not the mean.
A diagnostic model whose interval straddles zero has not demonstrated
improvement over the baseline, regardless of its point estimate.

The interval is a **moving block bootstrap** with circular blocks of length
n^(1/3), not an IID bootstrap. Signals minutes apart on one instrument, under a
48h holding limit, are not independent draws: resampling them individually
destroys exactly the dependence that drives the variance, and produces an
interval that is far too narrow. Each replicate resamples the same blocks of
*candidates* and recomputes both the accepted and the baseline mean from them,
so the correlation between the two estimates is preserved.

Reported alongside: the accepted set's expectancy interval (same method) and its
win rate as a **Wilson** interval — chosen because a win rate near 0 or 1 on a
few dozen trades is exactly where the normal approximation leaves [0,1] and
stops meaning anything.

All of it is deterministic: the bootstrap seed is fixed, so a training report is
reproducible.

## Champion–challenger

The incumbent is loaded and scored on the *same* held-out slice, using **its
own** calibration and **its own** threshold — comparing a challenger against a
champion at the challenger's threshold would be a rigged comparison. If the
incumbent cannot be loaded, that is logged as `champion_not_comparable` and the
comparison is skipped; it is never silently treated as beaten.

## Promotion

`auto_promote: true` (default) performs a transactional champion switch: lock
the candidate and the current ACTIVE row, retire, activate, audit. A partial
unique index enforces **exactly one ACTIVE model per objective**, so "which
model is production" is a fact rather than a convention.

`auto_promote: false` stops at APPROVED and waits for an operator.

## Rollback

Every model keeps its artifacts, its metrics, its walk-forward folds, its
calibration and its full event history. Rolling back is:

```sql
BEGIN;
UPDATE altsignalquality.models SET status = 'RETIRED', retired_at = now()
 WHERE status = 'ACTIVE' AND objective = 'binary:signal_quality';
UPDATE altsignalquality.models SET status = 'ACTIVE', activated_at = now(), retired_at = NULL
 WHERE model_id = '<the good one>';
INSERT INTO altsignalquality.model_events (model_id, event, detail, actor)
VALUES ('<the good one>', 'rolled_back', '{"reason":"..."}', '<operator>');
COMMIT;
```

The evaluator picks the change up on its next registry poll (30s), verifies the
artifact's checksum and feature-schema hash, and swaps the in-memory model. The
threshold and calibration roll back with it, because they belong to the model
row, not to the config.
