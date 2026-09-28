# Feature coverage

AltTech owns the feature definitions and publishes their versioned identities.
AltML archives eligible vectors for training and inference, then selects a
smaller point-in-time feature space for each model objective. AltML does not
recompute features that AltTech owns.

## Target and information venues

Bitget is the prediction target for the configured production models. Other
venues remain separate information sources: their values are identified by
venue and are never substituted for Bitget observations or combined into a
synthetic Bitget value.

The active feature sets, versions, timeframes, and member order are registered
by AltTech. The registry and each vector's schema hash are authoritative; the
set of available members can change as the producer versions its contract.

## Archive

`altml.feature_vectors` stores each vector with its exchange and instrument,
timeframe, bar time and revision, feature-set name and version, schema hash,
engine version, values, validity, quality, and receive/compute times.

Live inference archives vectors it consumes. A bounded backfill reads retained
AltTech feature vectors through the configured read-only database interface.
Both paths are idempotent, so the local archive supports the configured
training lookback independently of NATS retention.

Model feature selection is fold-local and recorded with the model's feature
compatibility metadata. See [Feature contract](FEATURE_CONTRACT.md) and
[Feature lineage](FEATURE_LINEAGE.md).
