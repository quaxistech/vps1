#include "normalization.hpp"

#include <cmath>

namespace alttech::crossvenue {

std::optional<NormalizedFunding> normalize_funding(double rate, double interval_hours) {
    if (!(interval_hours > 0.0) || !std::isfinite(rate) || !std::isfinite(interval_hours)) {
        return std::nullopt;
    }
    NormalizedFunding f;
    f.rate_per_hour = rate / interval_hours;
    f.rate_8h_equivalent = f.rate_per_hour * 8.0;
    f.annualized = f.rate_per_hour * 24.0 * 365.0;
    return f;
}

std::optional<double> oi_relative_change(double oi_usd, double previous_oi_usd) {
    if (!(previous_oi_usd > 0.0) || !std::isfinite(oi_usd)) return std::nullopt;
    return (oi_usd - previous_oi_usd) / previous_oi_usd;
}

double mean(std::span<const double> window) {
    if (window.empty()) return 0.0;
    double acc = 0.0;
    for (double v : window) acc += v;
    return acc / static_cast<double>(window.size());
}

double stddev(std::span<const double> window) {
    if (window.size() < 2) return 0.0;
    const double m = mean(window);
    double acc = 0.0;
    for (double v : window) {
        const double d = v - m;
        acc += d * d;
    }
    return std::sqrt(acc / static_cast<double>(window.size()));
}

std::optional<double> zscore(double x, std::span<const double> window) {
    if (window.size() < 2) return std::nullopt;
    const double s = stddev(window);
    if (!(s > 0.0)) return std::nullopt;
    return (x - mean(window)) / s;
}

double percentile_rank(double x, std::span<const double> window) {
    if (window.empty()) return 0.0;
    std::size_t le = 0;
    for (double v : window) {
        if (v <= x) ++le;
    }
    return static_cast<double>(le) / static_cast<double>(window.size());
}

} // namespace alttech::crossvenue
