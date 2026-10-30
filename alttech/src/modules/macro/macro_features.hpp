#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace alttech::modules::macro {

// The macro/cross-asset feature catalog.
//
// Ordered and frozen exactly as talib-core is: positions are the contract, and
// adding a name requires a new feature_set_version rather than an edit here.
//
// Three rules shape this list, and each of them removed features that looked
// attractive:
//
//   * Frequency follows the source. A daily series has no 5-minute return, so
//     none is published. Inventing one would hand the model a column that is
//     constant within a day and looks like signal at the boundary.
//
//   * Nothing here says what a number MEANS. There is no "CNH stress => risk
//     off", no regime label that encodes a direction. Regime appears only as
//     measured quantities -- a level, a change, a z-score -- and whether any of
//     them predicts anything is for the model to find, not for this file to
//     assert.
//
//   * The three CNY/CNH series stay apart. USDCNH is a traded offshore price,
//     the PBOC parity is a policy setting, and the Fed's H.10 is a reference
//     observation. Their DIFFERENCES are published as features precisely
//     because the three are not the same number; averaging them into one
//     "CNY" column would destroy the only interesting thing about them.
//
// Relative-to-crypto features (BTC vs gold, BTC vs NASDAQ) are deliberately
// NOT here. They would make this block per-instrument, multiplying identical
// macro rows across every pair, and they add nothing a model cannot derive: the
// crypto features and these sit in the same training row already.
const std::vector<std::string>& macro_feature_names();

[[nodiscard]] size_t macro_feature_count();

// sha256 of "name\n" concatenation, stored in feature_set_versions.schema_hash.
[[nodiscard]] std::string macro_schema_fingerprint();

inline constexpr const char* kMacroFeatureSetName = "macro-core";
inline constexpr int kMacroFeatureSetVersion = 1;
inline constexpr const char* kMacroMethodVersion = "mc-1.4.0";

} // namespace alttech::modules::macro
