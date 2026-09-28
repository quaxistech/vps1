#pragma once
// TA-Lib feature engine: every indicator TA-Lib ships, driven generically.
//
// Instead of hand-written indicator code this walks TA-Lib's *abstract*
// interface at start-up, enumerates each function it can satisfy from OHLCV
// with the library's own default parameters, and calls it in batch over the
// whole series. TA-Lib is the single source of indicator math.
//
// Excluded on purpose: the "Math Operators" and "Math Transform" groups —
// TA-Lib classifies those as arithmetic helpers (ADD/COS/LN/…), not indicators.
// Everything else is included: Overlap Studies, Momentum, Volume, Volatility,
// Price Transform, Cycle, Statistic and all 61 Pattern Recognition functions.

#include "alttech/common/error.hpp"

#include <memory>
#include <string>
#include <vector>

namespace alttech::talib_features {

// Column-major OHLCV, oldest bar first. `volume` may be empty when unavailable;
// functions requiring volume are then reported as all-NaN.
struct Ohlcv {
    std::vector<double> open;
    std::vector<double> high;
    std::vector<double> low;
    std::vector<double> close;
    std::vector<double> volume;

    size_t size() const { return close.size(); }
    bool consistent() const {
        return open.size() == close.size() && high.size() == close.size() &&
               low.size() == close.size() &&
               (volume.empty() || volume.size() == close.size());
    }
};

class Engine {
public:
    // Enumerates TA-Lib once. Fails only if the library refuses to initialize.
    static Result<std::unique_ptr<Engine>> create();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Stable, ordered feature names: "<FUNC>_<output>" (e.g. "RSI_real",
    // "MACD_macdsignal", "CDLDOJI_integer"). The order defines the vector layout.
    const std::vector<std::string>& names() const { return names_; }

    // Number of distinct TA-Lib functions behind those names.
    size_t function_count() const;

    // Longest warm-up (in bars) across all functions: rows before it are partly NaN.
    int max_lookback() const { return max_lookback_; }

    // rows[i] holds names().size() values for input bar i.
    // NaN marks "not computed here" (warm-up, or an input the function needs
    // that was not supplied) — absent values are never faked.
    Result<void> compute(const Ohlcv& in, std::vector<std::vector<double>>& rows) const;

private:
    Engine();
    struct Spec;
    std::vector<Spec> specs_;
    std::vector<std::string> names_;
    int max_lookback_ = 0;
};

// Process-wide engine. Enumeration is deterministic and immutable, so the
// catalog and the bound function set are shared instead of rebuilt per call.
// Returns nullptr only if TA-Lib refused to initialize.
const Engine* shared_engine();

} // namespace alttech::talib_features
