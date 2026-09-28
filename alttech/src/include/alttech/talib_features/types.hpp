#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace alttech::talib_features {

// The indicator math is TA-Lib 0.7.1 itself, driven through its abstract
// interface. The engine version is therefore the library version — there is no
// in-house formula to version.
//
// Coverage is complete by construction: every TA-Lib function is enumerated and
// bound, and the only exclusions are named individually in engine.cpp with the
// measurement that justifies them (functions whose output on a price series is
// NaN, infinite or constant on every bar).
inline constexpr const char* kEngineVersion = "talib-0.7.1";
// A brand-new durable namespace: this set has nothing in common with the old
// hand-written "core-realtime" vectors, so it must never be confused with them.
inline constexpr const char* kCoreSetName = "talib-core";
inline constexpr int kCoreSetVersion = 1;

// Bar as consumed by the engine. Analytics domain: double is acceptable here
// (money precision lives in NUMERIC candle storage; features are derivatives).
struct FBar {
    int64_t open_time_ms = 0;
    double open = 0;
    double high = 0;
    double low = 0;
    double close = 0;
    double base_volume = 0;
    double quote_volume = 0;
};

enum class VectorStatus { not_ready, warming, valid, incomplete_input, revised, invalid };

inline const char* status_label(VectorStatus s) {
    switch (s) {
        case VectorStatus::not_ready: return "NOT_READY";
        case VectorStatus::warming: return "WARMING";
        case VectorStatus::valid: return "VALID";
        case VectorStatus::incomplete_input: return "INCOMPLETE_INPUT";
        case VectorStatus::revised: return "REVISED";
        case VectorStatus::invalid: return "INVALID";
    }
    return "?";
}

struct FeatureVector {
    int64_t bar_open_time_ms = 0;
    std::vector<float> values;      // ordered by catalog position
    std::vector<uint8_t> validity;  // bitmap: bit i -> values[i] usable
    VectorStatus status = VectorStatus::not_ready;

    [[nodiscard]] bool is_valid(size_t i) const {
        return i / 8 < validity.size() && (validity[i / 8] >> (i % 8)) & 1;
    }
};

// Ordered catalog: one entry per TA-Lib output series, "<FUNC>_<output>"
// (e.g. "RSI_outReal", "MACD_outMACDSignal"). Positions are frozen once a
// feature_set_version row exists; additions require a new set version.
const std::vector<std::string>& core_feature_names();

// Number of feature columns (== core_feature_names().size()).
size_t feature_count();

// Bars of history replayed to warm every indicator up (TA-Lib's longest
// lookback plus margin), used on restart and for live trailing recompute.
size_t warmup_bars();

// sha256 of "name\n" concatenation — stored in feature_set_versions.schema_hash.
std::string core_schema_fingerprint();

} // namespace alttech::talib_features
