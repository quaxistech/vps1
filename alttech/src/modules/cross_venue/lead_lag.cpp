#include "lead_lag.hpp"

#include <cmath>
#include <cstdlib>

namespace alttech::crossvenue {

std::optional<double> correlation_at_lag(std::span<const double> a, std::span<const double> b,
                                         int lag) {
    // Align a[t] with b[t-lag]: iterate t so both indices are in range.
    const auto n_a = static_cast<long>(a.size());
    const auto n_b = static_cast<long>(b.size());
    long count = 0;
    double sa = 0.0;
    double sb = 0.0;
    double saa = 0.0;
    double sbb = 0.0;
    double sab = 0.0;
    for (long t = 0; t < n_a; ++t) {
        const long tb = t - lag;
        if (tb < 0 || tb >= n_b) continue;
        const double x = a[static_cast<std::size_t>(t)];
        const double y = b[static_cast<std::size_t>(tb)];
        sa += x;
        sb += y;
        saa += x * x;
        sbb += y * y;
        sab += x * y;
        ++count;
    }
    if (count < 2) return std::nullopt;
    const auto nc = static_cast<double>(count);
    const double cov = sab - sa * sb / nc;
    const double va = saa - sa * sa / nc;
    const double vb = sbb - sb * sb / nc;
    if (!(va > 0.0) || !(vb > 0.0)) return std::nullopt;
    return cov / std::sqrt(va * vb);
}

CrossLag best_cross_lag(std::span<const double> a, std::span<const double> b, int max_lag) {
    CrossLag best;
    double best_abs = -1.0;
    for (int lag = -max_lag; lag <= max_lag; ++lag) {
        const auto corr = correlation_at_lag(a, b, lag);
        if (!corr) continue;
        const double abs_corr = std::abs(*corr);
        if (abs_corr > best_abs) {
            best_abs = abs_corr;
            best.lag = lag;
            best.peak_corr = *corr;
        }
    }
    return best;
}

LeadLagResult compute_lead_lag(std::span<const VenueSeries> series, const LeadLagParams& params) {
    LeadLagResult result;
    const int n = static_cast<int>(series.size());

    // Pairwise directional lead accumulation, weighted by peak correlation.
    // For a pair (i, j): a positive lag of i vs j means i FOLLOWS j (j leads).
    std::array<double, 4> score{};
    std::array<double, 4> weight{};
    for (int i = 0; i < n; ++i) {
        const int bi = venue_bit(series[i].venue);
        if (bi < 0 || series[i].returns.size() < 2) continue;
        for (int j = i + 1; j < n; ++j) {
            const int bj = venue_bit(series[j].venue);
            if (bj < 0 || series[j].returns.size() < 2) continue;
            CrossLag cl = best_cross_lag(series[i].returns, series[j].returns, params.max_lag);
            if (std::abs(cl.peak_corr) < params.min_abs_corr) continue;

            // Correct false lead from clock skew: an apparent lag equal to the
            // relative clock offset is not real information flow. corrected =
            // measured + (clock_i - clock_j)/interval cancels a purely clock-
            // induced lag to zero.
            const double offset_samples =
                (series[i].clock_offset_ms - series[j].clock_offset_ms) /
                (params.sample_interval_ms > 0.0 ? params.sample_interval_ms : 1.0);
            const double corrected_lag = static_cast<double>(cl.lag) + offset_samples;

            const double w = std::abs(cl.peak_corr);
            // corrected_lag > 0: i follows j -> j leads (credit j), penalize i.
            score[static_cast<std::size_t>(bj)] += w * corrected_lag;
            score[static_cast<std::size_t>(bi)] -= w * corrected_lag;
            weight[static_cast<std::size_t>(bi)] += w;
            weight[static_cast<std::size_t>(bj)] += w;
        }
    }

    for (std::size_t b = 0; b < 4; ++b) {
        result.lead_score[b] = weight[b] > 0.0 ? score[b] / weight[b] : 0.0;
    }

    // Dominant leader = highest lead score with non-trivial confidence.
    double best = 0.0;
    for (const auto v : kAllVenues) {
        const auto b = static_cast<std::size_t>(venue_bit(v));
        if (weight[b] > 0.0 && result.lead_score[b] > best) {
            best = result.lead_score[b];
            result.dominant_leading_venue = v;
        }
    }
    double total_w = 0.0;
    for (double w : weight) total_w += w;
    result.lead_confidence = total_w > 0.0 ? std::min(1.0, best / static_cast<double>(std::max(1, params.max_lag))) : 0.0;

    // Bitget lag (ms) to each reference: positive => Bitget follows the venue.
    const auto find = [&](Venue v) -> const VenueSeries* {
        for (const auto& s : series) {
            if (s.venue == v) return &s;
        }
        return nullptr;
    };
    const VenueSeries* bg = find(Venue::bitget);
    const auto lag_ms_to = [&](Venue ref) -> double {
        const VenueSeries* rs = find(ref);
        if (!bg || !rs || bg->returns.size() < 2 || rs->returns.size() < 2) return 0.0;
        CrossLag cl = best_cross_lag(bg->returns, rs->returns, params.max_lag);
        if (std::abs(cl.peak_corr) < params.min_abs_corr) return 0.0;
        const double offset_samples =
            (bg->clock_offset_ms - rs->clock_offset_ms) /
            (params.sample_interval_ms > 0.0 ? params.sample_interval_ms : 1.0);
        return (static_cast<double>(cl.lag) + offset_samples) * params.sample_interval_ms;
    };
    result.bitget_lag_to_binance_ms = lag_ms_to(Venue::binance_usdm);
    result.bitget_lag_to_bybit_ms = lag_ms_to(Venue::bybit);
    result.bitget_lag_to_okx_ms = lag_ms_to(Venue::okx);
    return result;
}

} // namespace alttech::crossvenue
