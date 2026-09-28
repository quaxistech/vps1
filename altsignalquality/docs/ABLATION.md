# Ablation analysis

The optional `ablation` command compares training runs with selected feature
groups omitted. Each arm uses the same dataset, temporal splits, calibration
and promotion evaluation. Omitted features are marked missing, not set to zero.

```sh
altsignalquality ablation --help
```

Available groups are defined in the command implementation and include model
inputs such as ML estimates, cross-venue analytics, microstructure, higher-time
frame context, regimes and historical priors. The output is an experiment
report tied to its dataset version and feature schema; it is not a live signal
policy or a substitute for out-of-sample validation.

Training is disabled by default and quality scores do not gate signal
publication. Ablation is an optional model-analysis tool.
