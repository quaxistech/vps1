# Feature contract

AltML's hardest safety invariant: **the ordered feature vector the model was
trained on, the vector recorded in the registry, and the vector delivered to
live inference must be byte-for-byte the same** — same feature-set name and
version, same schema hash, same count, same order, same engine version. A
model is only served live when this holds.

## What's checked, and where

| Stage | Check | Where |
|---|---|---|
| Consuming a live vector | freshness (bar age, computation age), feature-set name/version/engine match, schema hash, feature count, validity bitmap shape, no non-finite valid value | `src/altml/components/inference_logic.hpp` |
| Training | the frozen feature set (`talib-core:1`, read through `postgres_fdw` from AltTech's catalog) is used as-is — the trainer never appends, drops or reorders a column; the recorded compatibility row carries the **live** schema hash + count by construction | `src/altml/components/trainer.cpp` |
| Promotion | feature compatibility and market-quality/champion-challenger gates before the transactional ACTIVE flip; economics are reported as diagnostics | `src/altml/components/trainer.cpp`, [PROMOTION_GATES.md](PROMOTION_GATES.md) |
| Live inference | booster feature count == registry count; schema hash match; a mismatch drops the vector, never zero-fills it | `src/altml/components/inference.cpp` |
| Audit CLI | reloads a model artifact and replays its recorded sample predictions | `altml model-validate` |
| Replay CLI | re-runs features → model → calibration → cost → decision for stored live predictions and compares each stage | `altml replay` |

## Current feature set

`talib-core:1` (`alttech.feature_set_versions`, read through
`altml.fdw_feature_set_versions`/`altml.fdw_feature_set_members`) — AltTech's
single-venue Bitget technical-indicator vector, engine version
`talib-0.7.1`. AltML has no builder step where an extra column could be
appended under someone else's schema hash: it reads the frozen set as
published and archives it as-is (see [DATABASE.md](DATABASE.md)).

Never disable feature-count validation, never keep a stale schema hash, and
never zero-fill a missing feature without treating it as invalid input.
