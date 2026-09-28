#pragma once

// The economic target, its calibration and the decision arithmetic. Pure: no
// I/O, no clock. Shared by the label builder, the trainer and inference so the
// three can never disagree about what a class, a cost or an expectation is.
//
// WHY THIS TARGET
//
// The model it replaces predicted direction_15m: sign(close[t+15] - close[t]).
// A two-basis-point move and a two-hundred-basis-point move were the same
// "up", while a round trip costs 16 bps on this venue (altml.fee_models v1:
// 2 x 6 bps taker + 2 x 2 bps slippage). A direction model can be right more
// often than not and still lose money on every trade it is right about.
//
// The target here asks the economic question instead, per horizon h:
//
//   LONG    a long opened at T0 and closed at T0+h clears the all-in cost
//   SHORT   a short opened at T0 and closed at T0+h clears the all-in cost
//   NEUTRAL neither does -- the move, whichever way, does not pay for itself
//
// The barrier is the cost model's round trip and nothing else. It is not a
// parameter searched for the best in-sample PnL (that search is forbidden:
// it fits the barrier to the sample), and it moves only when the cost model
// does, which versions the label set.
//
// A class probability is not an expectation of profit. The expected return of
// acting is computed separately, as the calibrated class probabilities weighted
// by what each class has been worth (class-conditional means from the rows the
// model was fitted on), minus the cost. Acting on a side requires that number
// to be positive -- the break-even rule of the cost model, not a tuned cut.

#include "altml/components/label_quality.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <numeric>
#include <random>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace altml::econ {

// ------------------------------------------------------------------ horizons

struct Horizon {
    std::string_view name;
    int minutes;
};

// The horizons a model may be trained for. Three, deliberately: a tactical
// one (15m -- the decision grid AltStrategy's setups live on), an intermediate
// one (1h) and a structural one (4h). Costs are fixed per round trip while
// dispersion grows with the square root of the horizon, so the same 16 bps is
// a 0.37-sigma hurdle at 15m and a 0.10-sigma hurdle at 4h (measured on 611k
// Bitget 15m decision points, Dec 2025 - Sep 2026).
inline constexpr std::array<Horizon, 3> kHorizons = {{{"15m", 15}, {"1h", 60}, {"4h", 240}}};
inline constexpr int kMaxHorizonMinutes = 240;

[[nodiscard]] inline std::optional<int> horizon_minutes(std::string_view name) {
    for (const auto& h : kHorizons) {
        if (h.name == name) return h.minutes;
    }
    return std::nullopt;
}

[[nodiscard]] inline std::optional<std::string_view> horizon_name(int minutes) {
    for (const auto& h : kHorizons) {
        if (h.minutes == minutes) return h.name;
    }
    return std::nullopt;
}

// ------------------------------------------------------------------- classes

enum Class : int { kShort = 0, kNeutral = 1, kLong = 2 };
inline constexpr int kClasses = 3;

// All-in round trip as a fraction of notional, from one altml.fee_models row:
// taker on both legs (a bar has no entry type, so the conservative half of the
// maker/taker pair) and the slippage estimate on both legs.
struct Cost {
    double fee_bps = 0;        // round trip
    double slippage_bps = 0;   // round trip
    [[nodiscard]] double bps() const { return fee_bps + slippage_bps; }
    [[nodiscard]] double fraction() const { return bps() / 10'000.0; }
};

[[nodiscard]] inline Cost cost_from_fee_model(double taker_fee, double slippage_estimate_bps) {
    return {2.0 * taker_fee * 10'000.0, 2.0 * slippage_estimate_bps};
}

// ------------------------------------------------------------- cost model v2
//
// The all-in round trip of a taker entry held for the horizon, estimated from
// what is known at T0 and nothing later:
//
//   fee      2 x the taker fee of altml.fee_models (both legs taker)
//   spread   the Bitget book spread OBSERVED at T0 (AltTech bitget_micro
//            book_spread_bps, the 1m window's mean): half is paid entering and
//            half leaving, so one full spread per round trip. Where no reading
//            exists the stated fallback is used and the estimate says so.
//   impact   an explicit per-side assumption (no fill history exists to fit it)
//
// v1 was one flat 16 bps for every instrument and moment. The observed Bitget
// spread runs from ~0.01 bps (BTC) to over 7 bps (p90 across the universe), so
// a flat figure over-charged the liquid names and under-charged the thin ones.
inline constexpr const char* kCostModelVersion = "cost-v2";

struct CostAt {
    double fee_rt_bps = 0, spread_bps = 0, impact_rt_bps = 0;
    bool spread_observed = false;
    [[nodiscard]] double bps() const { return fee_rt_bps + spread_bps + impact_rt_bps; }
    [[nodiscard]] double fraction() const { return bps() / 10'000.0; }
};

struct CostModelV2 {
    double fee_bps_per_side = 6.0;
    double impact_bps_per_side = 1.0;
    double fallback_spread_bps = 2.0;
    // A reading outside (0, max] is a broken book, not a cost: fallback.
    double max_plausible_spread_bps = 200.0;

    [[nodiscard]] CostAt at(double observed_spread_bps) const {
        CostAt c;
        c.fee_rt_bps = 2.0 * fee_bps_per_side;
        c.impact_rt_bps = 2.0 * impact_bps_per_side;
        c.spread_observed = std::isfinite(observed_spread_bps) && observed_spread_bps > 0.0 &&
                            observed_spread_bps <= max_plausible_spread_bps;
        c.spread_bps = c.spread_observed ? observed_spread_bps : fallback_spread_bps;
        return c;
    }
};

// Linear (USDT-margined) contract: a long earns the simple return, a short its
// negative. Log returns are what the labels store; the economics are simple.
[[nodiscard]] inline double simple(double log_return) { return std::expm1(log_return); }

[[nodiscard]] inline int econ_class(double log_return, double cost_fraction) {
    const double r = simple(log_return);
    if (r - cost_fraction > 0.0) return kLong;
    if (-r - cost_fraction > 0.0) return kShort;
    return kNeutral;
}

// ------------------------------------------------------------ path outcome

// What the market did over (T0, T0+h], from the anchor bar's close. Long
// perspective; the short side is the mirror (its favourable excursion is the
// long's adverse one), so storing both would store every number twice.
struct PathOutcome {
    double log_return = 0;   // log(close[t+h] / close[t])
    double mfe_long = 0;     // log(max high over t+1..t+h / close[t])
    double mae_long = 0;     // log(min low  over t+1..t+h / close[t])
    int minutes_to_mfe = 0;  // first minute the maximum high was printed
    int minutes_to_mae = 0;  // first minute the minimum low was printed
};

// Requires labels::source_range_valid(bars, anchor, h) -- the caller checks,
// because an incomplete path has no outcome at all rather than a partial one.
[[nodiscard]] inline PathOutcome path_outcome(const std::vector<labels::SourceBar>& bars,
                                              size_t anchor, int h) {
    PathOutcome o;
    const double c0 = bars[anchor].close;
    double hi = -std::numeric_limits<double>::infinity();
    double lo = std::numeric_limits<double>::infinity();
    for (int k = 1; k <= h; ++k) {
        const auto& b = bars[anchor + static_cast<size_t>(k)];
        if (b.high > hi) { hi = b.high; o.minutes_to_mfe = k; }
        if (b.low < lo) { lo = b.low; o.minutes_to_mae = k; }
    }
    o.log_return = std::log(bars[anchor + static_cast<size_t>(h)].close / c0);
    o.mfe_long = std::log(hi / c0);
    o.mae_long = std::log(lo / c0);
    return o;
}

// -------------------------------------------------------------- calibration

// Temperature scaling (Guo et al., 2017): one scalar T applied to the raw
// multiclass margins, softmax(z / T). One parameter is what a validation
// slice of a few thousand independent observations can support; per-class
// isotonic maps have far more freedom than that and fit its noise. T is fitted
// on the validation slice only -- never on the rows it is evaluated on -- and
// it is stored with the model and versioned with it.
inline constexpr const char* kCalibratorVersion = "temperature-1";

[[nodiscard]] inline std::array<double, 3> softmax3(const double* z, double temperature) {
    const double t = temperature > 0 ? temperature : 1.0;
    const double a = z[0] / t, b = z[1] / t, c = z[2] / t;
    const double m = std::max({a, b, c});
    const double ea = std::exp(a - m), eb = std::exp(b - m), ec = std::exp(c - m);
    const double s = ea + eb + ec;
    return {ea / s, eb / s, ec / s};
}

// Mean negative log-likelihood of `y` under softmax(raw / T). raw is n x 3.
[[nodiscard]] inline double nll_at(const std::vector<double>& raw, const std::vector<int>& y,
                                   double temperature) {
    if (y.empty() || raw.size() != y.size() * 3) return std::numeric_limits<double>::quiet_NaN();
    double acc = 0;
    for (size_t i = 0; i < y.size(); ++i) {
        const auto p = softmax3(&raw[i * 3], temperature);
        acc -= std::log(std::clamp(p[static_cast<size_t>(y[i])], 1e-12, 1.0));
    }
    return acc / static_cast<double>(y.size());
}

// Golden-section search on log T over [0.05, 20]. The NLL of temperature
// scaling is unimodal in T for a fixed set of margins, so a bracketing search
// needs no gradient and cannot wander.
[[nodiscard]] inline double fit_temperature(const std::vector<double>& raw,
                                            const std::vector<int>& y) {
    if (y.empty() || raw.size() != y.size() * 3) return 1.0;
    double lo = std::log(0.05), hi = std::log(20.0);
    const double g = (std::sqrt(5.0) - 1.0) / 2.0;
    double a = hi - g * (hi - lo), b = lo + g * (hi - lo);
    double fa = nll_at(raw, y, std::exp(a)), fb = nll_at(raw, y, std::exp(b));
    for (int it = 0; it < 60; ++it) {
        if (fa < fb) { hi = b; b = a; fb = fa; a = hi - g * (hi - lo); fa = nll_at(raw, y, std::exp(a)); }
        else         { lo = a; a = b; fa = fb; b = lo + g * (hi - lo); fb = nll_at(raw, y, std::exp(b)); }
    }
    return std::exp((lo + hi) / 2.0);
}

// ------------------------------------------------------------------ metrics

[[nodiscard]] inline std::array<double, 3> class_frequencies(const std::vector<int>& y,
                                                             const std::vector<int64_t>& idx) {
    std::array<double, 3> f{0, 0, 0};
    if (idx.empty()) return f;
    for (const int64_t i : idx) f[static_cast<size_t>(y[static_cast<size_t>(i)])] += 1.0;
    for (auto& v : f) v /= static_cast<double>(idx.size());
    return f;
}

// p is n x 3 row-major, aligned with y.
[[nodiscard]] inline double multi_logloss(const std::vector<double>& p, const std::vector<int>& y) {
    if (y.empty() || p.size() != y.size() * 3) return std::numeric_limits<double>::quiet_NaN();
    double acc = 0;
    for (size_t i = 0; i < y.size(); ++i) {
        acc -= std::log(std::clamp(p[i * 3 + static_cast<size_t>(y[i])], 1e-12, 1.0));
    }
    return acc / static_cast<double>(y.size());
}

[[nodiscard]] inline double multi_brier(const std::vector<double>& p, const std::vector<int>& y) {
    if (y.empty() || p.size() != y.size() * 3) return std::numeric_limits<double>::quiet_NaN();
    double acc = 0;
    for (size_t i = 0; i < y.size(); ++i) {
        for (int k = 0; k < 3; ++k) {
            const double o = y[i] == k ? 1.0 : 0.0;
            const double d = p[i * 3 + static_cast<size_t>(k)] - o;
            acc += d * d;
        }
    }
    return acc / static_cast<double>(y.size());
}

// The same scores for the base-rate forecast: every row gets the training
// class frequencies. Skill = 1 - model / climatology; > 0 means the model
// knows something the base rate does not.
[[nodiscard]] inline std::vector<double> climatology(const std::array<double, 3>& freq, size_t n) {
    std::vector<double> out(n * 3);
    for (size_t i = 0; i < n; ++i) {
        for (size_t k = 0; k < 3; ++k) out[i * 3 + k] = freq[k];
    }
    return out;
}

// Expected calibration error of one class's probability: equal-mass bins, the
// weighted gap between mean predicted probability and observed frequency.
[[nodiscard]] inline double ece_class(const std::vector<double>& p, const std::vector<int>& y,
                                      int k, int bins = 10) {
    const size_t n = y.size();
    if (n == 0 || p.size() != n * 3 || bins < 1) return std::numeric_limits<double>::quiet_NaN();
    std::vector<size_t> order(n);
    std::iota(order.begin(), order.end(), size_t{0});
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return p[a * 3 + static_cast<size_t>(k)] < p[b * 3 + static_cast<size_t>(k)];
    });
    double ece = 0;
    for (int b = 0; b < bins; ++b) {
        const size_t lo = n * static_cast<size_t>(b) / static_cast<size_t>(bins);
        const size_t hi = n * static_cast<size_t>(b + 1) / static_cast<size_t>(bins);
        if (hi <= lo) continue;
        double conf = 0, hit = 0;
        for (size_t j = lo; j < hi; ++j) {
            conf += p[order[j] * 3 + static_cast<size_t>(k)];
            hit += y[order[j]] == k ? 1.0 : 0.0;
        }
        const double m = static_cast<double>(hi - lo);
        ece += (m / static_cast<double>(n)) * std::abs(conf / m - hit / m);
    }
    return ece;
}

// ------------------------------------------------------------- expectations

// What each class has been worth, in simple-return terms, on the rows the model
// was FITTED on. Long perspective throughout.
struct ClassMeans {
    std::array<double, 3> ret{0, 0, 0};
    std::array<double, 3> mfe{0, 0, 0};
    std::array<double, 3> mae{0, 0, 0};
    std::array<int64_t, 3> n{0, 0, 0};

    [[nodiscard]] bool complete() const { return n[0] > 0 && n[1] > 0 && n[2] > 0; }
};

[[nodiscard]] inline ClassMeans class_means(const std::vector<int>& y,
                                            const std::vector<double>& log_ret,
                                            const std::vector<double>& log_mfe,
                                            const std::vector<double>& log_mae,
                                            const std::vector<int64_t>& idx) {
    ClassMeans m;
    for (const int64_t i64 : idx) {
        const auto i = static_cast<size_t>(i64);
        const auto k = static_cast<size_t>(y[i]);
        m.ret[k] += simple(log_ret[i]);
        m.mfe[k] += simple(log_mfe[i]);
        m.mae[k] += simple(log_mae[i]);
        ++m.n[k];
    }
    for (size_t k = 0; k < 3; ++k) {
        if (m.n[k] == 0) continue;
        const double d = static_cast<double>(m.n[k]);
        m.ret[k] /= d;
        m.mfe[k] /= d;
        m.mae[k] /= d;
    }
    return m;
}

// The model's estimate, in the units a decision is made in.
struct Estimate {
    double p_short = 0, p_neutral = 0, p_long = 0;
    double expected_return = 0;     // E[simple return over h], gross
    double expected_net_long = 0;   // E[return] - cost
    double expected_net_short = 0;  // -E[return] - cost
    double expected_mfe_long = 0;   // E[max favourable excursion], long
    double expected_mae_long = 0;   // E[max adverse excursion], long (<= 0)
};

[[nodiscard]] inline Estimate estimate(const std::array<double, 3>& p, const ClassMeans& m,
                                       double cost_fraction) {
    Estimate e;
    e.p_short = p[kShort];
    e.p_neutral = p[kNeutral];
    e.p_long = p[kLong];
    for (size_t k = 0; k < 3; ++k) {
        e.expected_return += p[k] * m.ret[k];
        e.expected_mfe_long += p[k] * m.mfe[k];
        e.expected_mae_long += p[k] * m.mae[k];
    }
    e.expected_net_long = e.expected_return - cost_fraction;
    e.expected_net_short = -e.expected_return - cost_fraction;
    return e;
}

// +1 long, -1 short, 0 stand aside. Both sides cannot be positive at once:
// their sum is -2 x cost.
[[nodiscard]] inline int decide_side(const Estimate& e, double margin_fraction = 0.0) {
    if (e.expected_net_long > margin_fraction) return 1;
    if (e.expected_net_short > margin_fraction) return -1;
    return 0;
}

// --------------------------------------------------------- economic replay

// One open position per instrument: a new entry on an instrument only after
// the previous one on it has been closed at its horizon. Different
// instruments may overlap -- they are different trades.
[[nodiscard]] inline std::vector<int64_t> select_per_instrument(
    const std::vector<int64_t>& idx, const std::vector<int64_t>& times_ms,
    const std::vector<int32_t>& symbols, const std::vector<int>& side, int64_t hold_ms) {
    std::vector<int64_t> order;
    for (const int64_t i : idx) {
        if (side[static_cast<size_t>(i)] != 0) order.push_back(i);
    }
    std::sort(order.begin(), order.end(), [&](int64_t a, int64_t b) {
        const auto ua = static_cast<size_t>(a), ub = static_cast<size_t>(b);
        if (symbols[ua] != symbols[ub]) return symbols[ua] < symbols[ub];
        if (times_ms[ua] != times_ms[ub]) return times_ms[ua] < times_ms[ub];
        return a < b;
    });
    std::vector<int64_t> out;
    int32_t current = std::numeric_limits<int32_t>::min();
    int64_t free_at = std::numeric_limits<int64_t>::min();
    for (const int64_t i : order) {
        const auto u = static_cast<size_t>(i);
        if (symbols[u] != current) { current = symbols[u]; free_at = std::numeric_limits<int64_t>::min(); }
        if (times_ms[u] >= free_at) {
            out.push_back(i);
            free_at = times_ms[u] + hold_ms;
        }
    }
    std::sort(out.begin(), out.end(), [&](int64_t a, int64_t b) {
        return times_ms[static_cast<size_t>(a)] < times_ms[static_cast<size_t>(b)] ||
               (times_ms[static_cast<size_t>(a)] == times_ms[static_cast<size_t>(b)] && a < b);
    });
    return out;
}

struct EconEval {
    int64_t trades = 0;
    double mean_net = std::numeric_limits<double>::quiet_NaN();    // per trade, fraction
    double mean_gross = std::numeric_limits<double>::quiet_NaN();
    double t_day = std::numeric_limits<double>::quiet_NaN();       // day-clustered t
    double hit_rate = std::numeric_limits<double>::quiet_NaN();
    double long_share = std::numeric_limits<double>::quiet_NaN();
    double profit_factor = std::numeric_limits<double>::quiet_NaN();
    double max_drawdown = std::numeric_limits<double>::quiet_NaN(); // of cumulative net, in trade-notional units
    double mean_mfe = std::numeric_limits<double>::quiet_NaN();    // side-adjusted
    double mean_mae = std::numeric_limits<double>::quiet_NaN();    // side-adjusted (<= 0)
    int64_t days = 0;
};

// Realised economics of the selected trades. Each trade is one unit of
// notional held for the horizon: an additive book, because positions on
// different instruments overlap and compounding them as if sequential would
// overstate both return and drawdown.
//
// The t statistic is clustered by UTC day: trades on the same day share the
// market's moves and are not independent observations.
// Per-row costs (cost model v2): each trade is charged the cost estimated at
// its own T0. `costs` is indexed like the other per-row vectors.
[[nodiscard]] inline EconEval evaluate(const std::vector<int64_t>& selected,
                                       const std::vector<int>& side,
                                       const std::vector<double>& log_ret,
                                       const std::vector<double>& log_mfe,
                                       const std::vector<double>& log_mae,
                                       const std::vector<int64_t>& times_ms,
                                       const std::vector<double>& costs);

[[nodiscard]] inline EconEval evaluate(const std::vector<int64_t>& selected,
                                       const std::vector<int>& side,
                                       const std::vector<double>& log_ret,
                                       const std::vector<double>& log_mfe,
                                       const std::vector<double>& log_mae,
                                       const std::vector<int64_t>& times_ms, double cost_fraction) {
    return evaluate(selected, side, log_ret, log_mfe, log_mae, times_ms,
                    std::vector<double>(log_ret.size(), cost_fraction));
}

[[nodiscard]] inline EconEval evaluate(const std::vector<int64_t>& selected,
                                       const std::vector<int>& side,
                                       const std::vector<double>& log_ret,
                                       const std::vector<double>& log_mfe,
                                       const std::vector<double>& log_mae,
                                       const std::vector<int64_t>& times_ms,
                                       const std::vector<double>& costs) {
    EconEval out;
    out.trades = static_cast<int64_t>(selected.size());
    if (selected.empty()) return out;
    constexpr int64_t kDay = 86'400'000;
    std::map<int64_t, double> by_day;
    double sum_net = 0, sum_gross = 0, gains = 0, losses = 0, cum = 0, peak = 0, dd = 0;
    double sum_mfe = 0, sum_mae = 0;
    int64_t wins = 0, longs = 0;
    for (const int64_t i64 : selected) {
        const auto i = static_cast<size_t>(i64);
        const double s = side[i] > 0 ? 1.0 : -1.0;
        const double gross = s * simple(log_ret[i]);
        const double net = gross - costs[i];
        sum_net += net;
        sum_gross += gross;
        if (net > 0) { ++wins; gains += net; } else { losses -= net; }
        if (s > 0) ++longs;
        sum_mfe += s > 0 ? simple(log_mfe[i]) : -simple(log_mae[i]);
        sum_mae += s > 0 ? simple(log_mae[i]) : -simple(log_mfe[i]);
        cum += net;
        peak = std::max(peak, cum);
        dd = std::max(dd, peak - cum);
        by_day[times_ms[i] / kDay] += net;
    }
    const double n = static_cast<double>(selected.size());
    out.mean_net = sum_net / n;
    out.mean_gross = sum_gross / n;
    out.hit_rate = static_cast<double>(wins) / n;
    out.long_share = static_cast<double>(longs) / n;
    out.profit_factor = losses > 0 ? gains / losses : std::numeric_limits<double>::quiet_NaN();
    out.max_drawdown = dd;
    out.mean_mfe = sum_mfe / n;
    out.mean_mae = sum_mae / n;
    out.days = static_cast<int64_t>(by_day.size());
    if (by_day.size() >= 3) {
        double m = 0;
        for (const auto& [_, v] : by_day) m += v;
        m /= static_cast<double>(by_day.size());
        double var = 0;
        for (const auto& [_, v] : by_day) var += (v - m) * (v - m);
        var /= static_cast<double>(by_day.size() - 1);
        if (var > 0) out.t_day = m / std::sqrt(var / static_cast<double>(by_day.size()));
    }
    return out;
}

// ------------------------------------------------------------ policy margin
//
// How far above its estimated cost an expected net must be before the model's
// policy acts on it. Chosen per model on its VALIDATION slice -- the slice the
// temperature is fitted on, which precedes the holdout -- from an a-priori
// grid, never on the holdout and never on the data the model trained on. The
// holdout is then scored with the chosen margin. A validation slice on which
// no margin produces kMinValidationTrades trades with a POSITIVE mean net
// yields NO margin: the model's estimates are recorded, and nothing it says is
// actionable.
inline constexpr const char* kPolicyVersion = "policy-2";
inline constexpr std::array<double, 5> kMarginGridBps = {0.0, 2.0, 5.0, 10.0, 20.0};
inline constexpr int64_t kMinValidationTrades = 30;

struct MarginChoice {
    std::optional<double> margin_bps;
    int64_t trades = 0;
    double mean_net_bps = std::numeric_limits<double>::quiet_NaN();
};

// `est` is aligned with `idx`; costs, times, symbols and outcomes are full-row.
[[nodiscard]] inline MarginChoice select_margin(const std::vector<int64_t>& idx,
                                                const std::vector<Estimate>& est,
                                                const std::vector<int64_t>& times_ms,
                                                const std::vector<int32_t>& symbols,
                                                const std::vector<double>& log_ret,
                                                const std::vector<double>& log_mfe,
                                                const std::vector<double>& log_mae,
                                                const std::vector<double>& costs, int64_t hold_ms) {
    MarginChoice best;
    for (const double m : kMarginGridBps) {
        std::vector<int> side(log_ret.size(), 0);
        for (size_t i = 0; i < idx.size(); ++i) {
            side[static_cast<size_t>(idx[i])] = decide_side(est[i], m / 10'000.0);
        }
        const auto sel = select_per_instrument(idx, times_ms, symbols, side, hold_ms);
        if (static_cast<int64_t>(sel.size()) < kMinValidationTrades) continue;
        const auto e = evaluate(sel, side, log_ret, log_mfe, log_mae, times_ms, costs);
        const double net_bps = e.mean_net * 10'000.0;
        // A margin is VALIDATED only where acting on it paid on the validation
        // slice. The least-bad of several losing margins is not a margin: it
        // would mark as actionable a model whose own validation says it loses.
        if (!(net_bps > 0.0)) continue;
        if (!best.margin_bps || net_bps > best.mean_net_bps) {
            best.margin_bps = m;
            best.trades = e.trades;
            best.mean_net_bps = net_bps;
        }
    }
    return best;
}

// ------------------------------------------------------------------- gates

struct SliceScore {
    double logloss_skill = std::numeric_limits<double>::quiet_NaN();
    double brier_skill = std::numeric_limits<double>::quiet_NaN();
    double direction_auc = std::numeric_limits<double>::quiet_NaN();
    double ece_long = std::numeric_limits<double>::quiet_NaN();
    double ece_short = std::numeric_limits<double>::quiet_NaN();
    EconEval econ;
};

struct GateConfig {
    int64_t min_trades = 50;
    // A one-sided day-clustered t of 2.0 is p ~ 0.023 for one horizon; three
    // horizons are tested, so the family-wise rate is ~0.07 (Bonferroni). It
    // is the multiple-testing-aware version of "the mean is positive", not a
    // number tuned to let a model through.
    double min_holdout_t = 2.0;
    // Of the walk-forward folds, how many must be net positive. With four
    // folds, three.
    int64_t min_positive_folds = 3;
};

// ---------------------------------------------------- rank statistics

// Average ranks (1-based), ties sharing the mean of their positions.
[[nodiscard]] inline std::vector<double> average_ranks(const std::vector<double>& v) {
    std::vector<size_t> o(v.size());
    std::iota(o.begin(), o.end(), size_t{0});
    std::sort(o.begin(), o.end(), [&](size_t a, size_t b) { return v[a] < v[b]; });
    std::vector<double> r(v.size());
    for (size_t i = 0; i < o.size();) {
        size_t j = i;
        while (j + 1 < o.size() && v[o[j + 1]] == v[o[i]]) ++j;
        const double avg = (static_cast<double>(i) + static_cast<double>(j)) / 2.0 + 1.0;
        for (size_t k = i; k <= j; ++k) r[o[k]] = avg;
        i = j + 1;
    }
    return r;
}

// Spearman rank correlation over the pairs where both values are finite.
[[nodiscard]] inline double spearman(const std::vector<double>& a, const std::vector<double>& b) {
    std::vector<double> x, y;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
        if (std::isfinite(a[i]) && std::isfinite(b[i])) { x.push_back(a[i]); y.push_back(b[i]); }
    }
    if (x.size() < 3) return std::numeric_limits<double>::quiet_NaN();
    const auto rx = average_ranks(x), ry = average_ranks(y);
    const double n = static_cast<double>(rx.size());
    double mx = 0, my = 0;
    for (size_t i = 0; i < rx.size(); ++i) { mx += rx[i]; my += ry[i]; }
    mx /= n; my /= n;
    double sxy = 0, sxx = 0, syy = 0;
    for (size_t i = 0; i < rx.size(); ++i) {
        sxy += (rx[i] - mx) * (ry[i] - my);
        sxx += (rx[i] - mx) * (rx[i] - mx);
        syy += (ry[i] - my) * (ry[i] - my);
    }
    return sxx > 0 && syy > 0 ? sxy / std::sqrt(sxx * syy) : std::numeric_limits<double>::quiet_NaN();
}

// Directional skill: the AUC of (P(long) - P(short)) for "the return was up"
// over the rows whose return was not exactly zero (Mann-Whitney, ties halved).
// 0.5 is no directional information, whatever the log-loss skill says -- a
// model can beat the base rates on volatility alone.
[[nodiscard]] inline double direction_auc(const std::vector<double>& p, const std::vector<double>& log_ret) {
    std::vector<double> score;
    std::vector<int> up;
    for (size_t i = 0; i < log_ret.size() && i * 3 + 2 < p.size(); ++i) {
        if (!std::isfinite(log_ret[i]) || log_ret[i] == 0.0) continue;
        score.push_back(p[i * 3 + 2] - p[i * 3]);
        up.push_back(log_ret[i] > 0 ? 1 : 0);
    }
    const auto r = average_ranks(score);
    double pos = 0, rank_sum = 0;
    for (size_t i = 0; i < up.size(); ++i) {
        if (up[i]) { pos += 1; rank_sum += r[i]; }
    }
    const double neg = static_cast<double>(up.size()) - pos;
    if (pos < 1 || neg < 1) return std::numeric_limits<double>::quiet_NaN();
    return (rank_sum - pos * (pos + 1) / 2.0) / (pos * neg);
}

// ------------------------------------------------ VPS1 market-quality gates

// VPS1 is the MARKET-INTELLIGENCE layer: a model is promoted when it predicts
// the market better than the base rates, out of sample and consistently --
// proper scores only. Whether the predicted move pays after today's costs is
// VPS2's question, so the economics are reported with every model and gate
// nothing (gate_failures() below kept that veto in VPS1 and rejected every
// model on 2026-09-25 while each forecast the market better than climatology).
struct MarketGateConfig {
    // Of the walk-forward folds, how many must beat climatology on log-loss.
    // With four folds, three: a majority that a single lucky month cannot make.
    int64_t min_positive_folds = 3;
};

[[nodiscard]] inline std::vector<std::string> market_gate_failures(const std::vector<SliceScore>& folds,
                                                                   const SliceScore& holdout,
                                                                   const MarketGateConfig& g) {
    std::vector<std::string> f;
    double ll = 0;
    int64_t positive = 0, scored = 0;
    for (const auto& s : folds) {
        if (!std::isfinite(s.logloss_skill)) continue;
        ++scored;
        ll += s.logloss_skill;
        if (s.logloss_skill > 0) ++positive;
    }
    if (scored == 0) {
        f.emplace_back("no scored walk-forward fold");
        return f;
    }
    if (!(ll / static_cast<double>(scored) > 0)) f.emplace_back("mean fold log-loss skill <= 0");
    if (positive < g.min_positive_folds) {
        f.emplace_back("folds beating climatology " + std::to_string(positive) + "/" + std::to_string(scored));
    }
    if (!(holdout.logloss_skill > 0)) f.emplace_back("holdout log-loss skill <= 0");
    if (!(holdout.brier_skill > 0)) f.emplace_back("holdout Brier skill <= 0");
    return f;
}

// ------------------------------------------------------- calibration test

// Is a probability a probability? The holdout ECE of each class is compared
// with the ECE a PERFECTLY calibrated forecaster would show on the same rows:
// labels drawn from the forecast itself, `sims` times, deterministic seed. The
// class passes when its ECE is within the 95th percentile of that null -- the
// ECE is then indistinguishable from sampling noise. A model whose classes do
// not all pass publishes its numbers as uncalibrated scores and says so.
inline constexpr const char* kCalibrationTestVersion = "ece-null-q95-1";

struct CalibrationTest {
    std::array<double, 3> ece{std::nan(""), std::nan(""), std::nan("")};
    std::array<double, 3> null_q95{std::nan(""), std::nan(""), std::nan("")};
    int sims = 0;
    bool passed = false;
};

[[nodiscard]] inline CalibrationTest calibration_test(const std::vector<double>& p, const std::vector<int>& y,
                                                      int bins = 10, int sims = 200,
                                                      uint64_t seed = 0x5eed'ca1bULL) {
    CalibrationTest t;
    const size_t n = y.size();
    if (n < static_cast<size_t>(bins) * 10 || p.size() != n * 3 || sims < 20) return t;
    t.sims = sims;
    // Each class's rows in ascending forecast order, fixed across simulations.
    std::array<std::vector<size_t>, 3> order;
    for (size_t k = 0; k < 3; ++k) {
        order[k].resize(n);
        std::iota(order[k].begin(), order[k].end(), size_t{0});
        std::sort(order[k].begin(), order[k].end(), [&](size_t a, size_t b) {
            return p[a * 3 + k] < p[b * 3 + k] || (p[a * 3 + k] == p[b * 3 + k] && a < b);
        });
    }
    const auto ece_of = [&](size_t k, const std::vector<int>& lab) {
        double e = 0;
        for (int b = 0; b < bins; ++b) {
            const size_t lo = n * static_cast<size_t>(b) / static_cast<size_t>(bins);
            const size_t hi = n * static_cast<size_t>(b + 1) / static_cast<size_t>(bins);
            if (hi <= lo) continue;
            double conf = 0, hit = 0;
            for (size_t j = lo; j < hi; ++j) {
                const size_t i = order[k][j];
                conf += p[i * 3 + k];
                hit += lab[i] == static_cast<int>(k) ? 1.0 : 0.0;
            }
            const double m = static_cast<double>(hi - lo);
            e += (m / static_cast<double>(n)) * std::abs(conf / m - hit / m);
        }
        return e;
    };
    for (size_t k = 0; k < 3; ++k) t.ece[k] = ece_of(k, y);
    std::array<std::vector<double>, 3> null;
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    std::vector<int> draw(n);
    for (int s = 0; s < sims; ++s) {
        for (size_t i = 0; i < n; ++i) {
            const double x = u(rng);
            draw[i] = x < p[i * 3] ? 0 : (x < p[i * 3] + p[i * 3 + 1] ? 1 : 2);
        }
        for (size_t k = 0; k < 3; ++k) null[k].push_back(ece_of(k, draw));
    }
    t.passed = true;
    for (size_t k = 0; k < 3; ++k) {
        auto& v = null[k];
        std::sort(v.begin(), v.end());
        t.null_q95[k] = v[static_cast<size_t>(0.95 * static_cast<double>(v.size() - 1))];
        if (!(t.ece[k] <= t.null_q95[k])) t.passed = false;
    }
    return t;
}

// ---------------------------------------------------------- legacy gates

// The economic promotion gates of policy-2, kept only so their verdict can be
// REPORTED beside a model (diagnostic); VPS1 promotes on market_gate_failures.
// Every gate is a proper-scoring or economic statement; none reads accuracy.
[[nodiscard]] inline std::vector<std::string> gate_failures(const std::vector<SliceScore>& folds,
                                                            const SliceScore& holdout,
                                                            const GateConfig& g) {
    std::vector<std::string> f;
    double ll = 0, net = 0;
    int64_t positive = 0, scored = 0;
    for (const auto& s : folds) {
        if (!std::isfinite(s.logloss_skill)) continue;
        ++scored;
        ll += s.logloss_skill;
        const double m = std::isfinite(s.econ.mean_net) ? s.econ.mean_net : 0.0;
        net += m;
        if (s.econ.trades > 0 && m > 0) ++positive;
    }
    if (scored == 0) {
        f.emplace_back("no scored walk-forward fold");
        return f;
    }
    if (!(ll / static_cast<double>(scored) > 0)) f.emplace_back("mean fold log-loss skill <= 0");
    if (!(net / static_cast<double>(scored) > 0)) f.emplace_back("mean fold net expectancy <= 0");
    if (positive < g.min_positive_folds) {
        f.emplace_back("net-positive folds " + std::to_string(positive) + "/" +
                       std::to_string(scored));
    }
    if (!(holdout.logloss_skill > 0)) f.emplace_back("holdout log-loss skill <= 0");
    if (!(holdout.brier_skill > 0)) f.emplace_back("holdout Brier skill <= 0");
    if (holdout.econ.trades < g.min_trades) {
        f.emplace_back("holdout trades " + std::to_string(holdout.econ.trades) + " < " +
                       std::to_string(g.min_trades));
    }
    if (!(holdout.econ.mean_net > 0)) f.emplace_back("holdout net expectancy <= 0");
    if (!(holdout.econ.t_day >= g.min_holdout_t)) f.emplace_back("holdout day-clustered t below minimum");
    return f;
}

} // namespace altml::econ
