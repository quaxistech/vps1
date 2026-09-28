# Feature contract

The ordered list in `src/altsignalquality/domain/feature_contract.hpp` is the
source of truth for model inputs. Current identity: **`sq-core:6`, 109
features**.

Feature order is part of the contract because vectors are positional. The
service hashes the feature definitions; evaluations, training examples and
models record the schema version and hash. A model with a different identity
cannot be used by the running contract. Any change to feature order or meaning
requires a new schema version.

## Input groups

| source | contents |
|---|---|
| `proposal` | Trade plan, side, proposal metadata, available economic estimates and risk/context indicators carried by AltStrategy |
| `derived` | Ratios and quantities calculated from the proposal and point-in-time context |
| `context_snapshot` | Scalar market and execution context stored for the proposal |
| `context_analytics` | Named analytics stored in the point-in-time context snapshot |
| `prior` | Historical outcomes available as of the proposal timestamp |

Every feature declares `AT_SIGNAL_TIME` availability. The leakage audit checks
that declaration before training and through `altsignalquality validate`.

## Shared assembly and missing values

Training and inference call the same feature assembler. A live evaluation
stores the vector used at signal time; its training example reuses that vector.
Backfill reconstructs the vector from AltStrategy's point-in-time archive.

An unavailable input is represented as missing, with its validity bit cleared.
It is not zero-filled, mean-filled or carried forward. A categorical value
outside the declared vocabulary is also missing. This preserves the difference
between an observed zero and an unavailable value.

## Diagnostics

Dataset diagnostics report observed and missing counts, summary statistics,
constant columns and duplicate columns. These reports describe the data; they
do not change the feature contract. Optional model scores are diagnostic and do
not determine whether an otherwise-valid signal is published.

Run the contract and dataset audit with:

```sh
altsignalquality validate --config /etc/altsignalquality/altsignalquality.yaml
```
