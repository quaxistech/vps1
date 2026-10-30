#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace alttech::modules::macro {

// One minute of an intraday series, as AltMacro stored it.
struct IntradayPoint {
    int64_t t_ms = 0;          // bar open, UTC, on the 1m grid
    double mid = 0.0;          // (bid + ask) / 2
    double spread_bps = 0.0;   // (ask - bid) / mid, in basis points
    // False when AltMacro marked the minute STALE -- the feed repeated the last
    // price because no tick arrived. The level is still the best estimate of
    // where the market is, so it is used; but a return computed ACROSS such a
    // stretch is an artefact of the carry-forward, not a move, and the
    // computation below refuses to publish one.
    bool traded = false;
    int64_t known_at_ms = 0; // actual max receipt/update of both quote sides
};

// One immutable daily revision. Observation identifies the described period;
// known_at identifies when this exact revision became available to its owner.
struct DailyPoint {
    int64_t observation_start_ms = 0;
    double value = 0.0;
    // Real owner availability of this exact revision. Zero is reserved for
    // legacy explicitly constructed fixtures; runtime loaders always supply it.
    int64_t known_at_ms = 0;
    int64_t revision = 0;
    bool valid = true;
    int64_t version_at_ms = 0; // source vintage/revision order, distinct from receipt
};

// Everything the macro block reads, already fetched and sorted ascending.
struct MacroInputs {
    std::vector<IntradayPoint> usdcnh;
    std::vector<IntradayPoint> eurusd;
    std::vector<IntradayPoint> usdjpy;
    std::vector<IntradayPoint> xauusd;

    std::vector<DailyPoint> fixing;        // REF.USDCNY.PBOC
    std::vector<DailyPoint> h10_cny;       // REF.USDCNY.FED
    std::vector<DailyPoint> gbpusd;        // REF.GBPUSD.FED
    std::vector<DailyPoint> audusd;        // REF.AUDUSD.FED
    std::vector<DailyPoint> usdkrw;        // REF.USDKRW.FED
    std::vector<DailyPoint> vix;
    std::vector<DailyPoint> spx;
    std::vector<DailyPoint> ndx;
    std::vector<DailyPoint> us2y;
    std::vector<DailyPoint> us10y;
    std::vector<DailyPoint> us10y_real;

    // One offshore mid per day, taken shortly after the PBOC fix publishes, so
    // the fix-gap z-score can be primed from history without loading a month
    // of minute bars. The rolling z-scores are DAILY-cadence statistics: they
    // need twenty observations before they mean anything, and a live cycle
    // that sees four hours of minutes plus two days of warm-up can never
    // accumulate that from the minute loop alone -- which is how both z-score
    // positions sat at zero validity for the whole first deployment.
    std::vector<DailyPoint> cnh_daily_mid;
};

enum class VectorStatus { warming, valid, incomplete_input };

struct MacroVector {
    int64_t t_ms = 0;
    std::vector<float> values;
    std::vector<uint8_t> validity;   // bit i set -> values[i] usable
    VectorStatus status = VectorStatus::warming;

    [[nodiscard]] bool is_valid(size_t i) const {
        return i / 8 < validity.size() && ((validity[i / 8] >> (i % 8)) & 1u) != 0;
    }
};

// Compute the macro block on the 1m grid over [from_ms, to_ms).
//
// Every value is derived only from observations that were usable at that
// minute: intraday bars that had already closed, and daily figures whose
// publication lag had elapsed. Nothing reads forward.
//
// A feature whose inputs are missing, stale beyond use, or not yet published is
// left with its validity bit CLEAR and its value zero. The zero is never a
// measurement -- the bitmap is what says whether to look at it, exactly as in
// talib-core.
[[nodiscard]] std::vector<MacroVector>
compute_macro_series(const MacroInputs& inputs, int64_t from_ms, int64_t to_ms);

// Exposed for tests: the two constructions with one correct definition.
[[nodiscard]] inline double basis_points(double a, double b) {
    return b == 0.0 ? 0.0 : (a - b) / b * 10'000.0;
}

} // namespace alttech::modules::macro
