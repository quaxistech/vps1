# Labels

Label contract: **`sq-label:1`**, defined in
`src/altsignalquality/domain/labeling.hpp`.

AltStrategy owns market-outcome reconstruction. AltSignalQuality reads its
resolved outcome and defines a binary training label; it does not rebuild the
price path or infer an outcome that AltStrategy marked unusable.

## Label rule

```text
label = 1  if  net_return_after_costs > 0
label = 0  otherwise
```

Only resolved outcomes that took a position are eligible:
`TARGET_FIRST`, `STOP_FIRST` and `TIME_STOP`. The cost-adjusted return is
supplied by AltStrategy. Outcome category alone does not determine the label:
a target may have negative net return after costs, and a time stop may have
positive net return.

## Excluded outcomes

- `AMBIGUOUS` and `DATA_INCOMPLETE`: the price path does not establish which
  exit occurred first, or the upstream label is marked unusable.
- `ENTRY_NOT_REACHED`, `ENTRY_EXPIRED` and `CANCELLED`: no position was opened,
  so there is no realized trade outcome to label.
- Missing or non-finite `net_return_after_costs`.

The dataset also uses only proposal geometries published on the input subject
and applies the configured `wiring.max_stop_distance_bps` data-selection bound.
Neither condition changes the treatment of live signals.

## Maturation and point-in-time use

An outcome is eligible only after its label-maturation timestamp. Priors read
only outcomes that matured by the proposal's signal timestamp. Training folds
purge examples whose labels mature inside a validation or test interval; see
[LEAKAGE.md](LEAKAGE.md).

The label definition is versioned in `altsignalquality.label_specs`. Changing
the eligible outcomes or positive-label rule requires a new label-spec version;
existing examples retain the version under which they were created.
