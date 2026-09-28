# Point-in-time data rules

A historical decision must use only information available at its decision time. This document states the rules for downstream features, predictions, and outcomes that use AltSpotter data.

## Decision time

T0 is the timestamp at which a decision context is captured. Derived records retain that timestamp or a reference to the immutable context that contains it.

## Allowed at T0

- Closed market bars whose event time is at or before T0.
- Features derived from those bars and current, according to their configured freshness window.
- The last observed order-book snapshot at or before T0, when it is within the configured freshness window.
- Model and adaptive state that was active and available at T0.
- Outcomes that fully resolved before T0.

## Forbidden at T0

- Market observations, features, or predictions timestamped after T0.
- Current market state substituted for historical state during replay.
- Outcomes from the decision being evaluated, or information derived from those outcomes.
- Receive or persistence timestamps used as market features.
- Labels whose target horizon extends into the feature or decision window.

## Missing observations

Missing values remain explicit. A fallback value must be distinguishable from an observed market value, and an invalid or incomplete label must not be represented as zero.

## Historical replay

Replay reconstructs the state that existed at T0 using event timestamps, revisions, and the model version active at that time. If required historical data is not retained, the replay records the observation as unavailable instead of substituting current data.

See [Temporal model](TEMPORAL_MODEL.md) for timestamp definitions and [Market-data semantics](../MARKET_DATA_SEMANTICS.md) for AltSpotter field behavior.
