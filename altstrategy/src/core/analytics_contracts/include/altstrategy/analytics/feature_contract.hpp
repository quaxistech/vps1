#pragma once

namespace altstrategy::analytics {

// Wire/storage contracts produced by the external Alttech analytics pipeline.
// Bumping any value requires a coordinated alttech + consumer rollout.
inline constexpr const char* kFeatureEngineVersion = "talib-0.7.1";
inline constexpr const char* kCoreFeatureSetName = "talib-core";
inline constexpr int kCoreFeatureSetVersion = 1;
inline constexpr const char* kStructureAlgorithmVersion = "st-1.1.0";
inline constexpr const char* kRegimeAlgorithmVersion = "rg-1.1.0";

} // namespace altstrategy::analytics
