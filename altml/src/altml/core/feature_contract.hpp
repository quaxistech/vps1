#pragma once

namespace altml::analytics {

// Wire/storage contracts produced by the external Alttech analytics pipeline.
// Bumping any value requires a coordinated alttech + consumer rollout.
inline constexpr const char* kFeatureEngineVersion = "talib-0.7.1";
inline constexpr const char* kCoreFeatureSetName = "talib-core";
inline constexpr int kCoreFeatureSetVersion = 1;
inline constexpr const char* kStructureAlgorithmVersion = "st-1.1.0";
inline constexpr const char* kRegimeAlgorithmVersion = "rg-1.1.0";

// The single trading/execution venue.
//
// Bitget is the TARGET: every label, every prediction and every economic
// evaluation is about a Bitget instrument on the Bitget 1m grid. Binance, OKX
// and Bybit are INFORMATION venues -- archived as evidence and usable as model
// inputs when point-in-time correct, but never a target and never a substitute
// for the target's own observation.
//
// This value appears as the literal `1` in label_builder.cpp's SQL (8 sites,
// where turning compile-time query strings into runtime concatenation would
// cost more than it explains) and as EXCHANGE_BITGET in codec.cpp. codec.cpp
// carries a static_assert tying the two together, so they cannot drift.
inline constexpr int kTargetExchangeId = 1;
inline constexpr const char* kTargetTimeframe = "1m";

// Version of the AltML -> AltStrategy prediction message. Bump on any wire
// change; see altstrategy/docs/PREDICTION_CONTRACT.md.
// 2: econ models. probability_up/down are the LONG/SHORT class probabilities
// of the econ target (a side clears the all-in cost over the horizon), with
// the neutral remainder, the expected net return per side, the horizon and
// the calibrator stated explicitly (see proto/prediction.proto).
// 3: + the decision cost by component (cost model v2: fee, spread observed at
// T0, impact), the model's validated policy margin, and favourable/adverse
// magnitudes per side.
// 4: market-prediction semantics (VPS1 = market intelligence): whether the
// class probabilities passed the holdout calibration test, what up / flat /
// down mean, the expected up / down excursions as magnitudes and their
// source, the model's role (ACTIVE / SHADOW). Consumers still read 3.
inline constexpr int kPredictionSchemaVersion = 4;

} // namespace altml::analytics
