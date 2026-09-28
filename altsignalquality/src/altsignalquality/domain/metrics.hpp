#pragma once

// Metrics for a signal FILTER, not for a classifier.
//
// Accuracy is close to meaningless here: a filter that accepts nothing is 100%
// "right" about every rejection, and a base rate of 65% wins makes "accept
// everything" a 65%-accurate model. What matters is whether the ACCEPTED set is
// economically better than the set that would have been taken anyway, and
// whether that holds across time, strategies, symbols and regimes rather than
// in aggregate only.
//
// So `FilterEvaluation` always reports the accepted set NEXT TO the baseline
// (all candidate signals) and the uplift between them, and the promotion gates
// read the uplift, never the raw score.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

namespace altsignalquality::domain {

// ROC-AUC by rank statistic; ties get the average rank. NaN if one class is
// absent (an honest "undefined", never 0.5).
[[nodiscard]] inline double roc_auc(const std::vector<int>& y, const std::vector<double>& p) {
    if (y.size() != p.size() || y.empty()) return std::nan("");
    std::vector<size_t> order(y.size());
    std::iota(order.begin(), order.end(), size_t{0});
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return p[a] < p[b]; });
    double rank_sum_pos = 0;
    size_t n_pos = 0, n_neg = 0, i = 0;
    while (i < order.size()) {
        size_t j = i;
        while (j + 1 < order.size() && p[order[j + 1]] == p[order[i]]) ++j;
        const double avg_rank = (static_cast<double>(i) + static_cast<double>(j)) / 2.0 + 1.0;
        for (size_t k = i; k <= j; ++k) {
            if (y[order[k]] == 1) {
                rank_sum_pos += avg_rank;
                ++n_pos;
            } else {
                ++n_neg;
            }
        }
        i = j + 1;
    }
    if (n_pos == 0 || n_neg == 0) return std::nan("");
    return (rank_sum_pos - static_cast<double>(n_pos) * (static_cast<double>(n_pos) + 1) / 2.0) /
           (static_cast<double>(n_pos) * static_cast<double>(n_neg));
}

// Average precision — the area under the precision/recall curve by the
// step-wise sum used in the literature. More informative than ROC-AUC when the
// positive class is what you act on and the classes are unbalanced.
[[nodiscard]] inline double pr_auc(const std::vector<int>& y, const std::vector<double>& p) {
    if (y.size() != p.size() || y.empty()) return std::nan("");
    std::vector<size_t> order(y.size());
    std::iota(order.begin(), order.end(), size_t{0});
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return p[a] > p[b]; });
    const auto total_pos = static_cast<double>(std::count(y.begin(), y.end(), 1));
    if (total_pos == 0.0) return std::nan("");
    double tp = 0, fp = 0, prev_recall = 0, ap = 0;
    size_t i = 0;
    while (i < order.size()) {
        size_t j = i;
        while (j + 1 < order.size() && p[order[j + 1]] == p[order[i]]) ++j;
        for (size_t k = i; k <= j; ++k) (y[order[k]] == 1 ? tp : fp) += 1.0;
        const double recall = tp / total_pos;
        const double precision = tp / (tp + fp);
        ap += (recall - prev_recall) * precision;
        prev_recall = recall;
        i = j + 1;
    }
    return ap;
}

[[nodiscard]] inline double log_loss(const std::vector<int>& y, const std::vector<double>& p) {
    if (y.size() != p.size() || y.empty()) return std::nan("");
    double acc = 0;
    for (size_t i = 0; i < y.size(); ++i) {
        const double q = std::clamp(p[i], 1e-15, 1.0 - 1e-15);
        acc -= y[i] == 1 ? std::log(q) : std::log(1.0 - q);
    }
    return acc / static_cast<double>(y.size());
}

[[nodiscard]] inline double brier_score(const std::vector<int>& y, const std::vector<double>& p) {
    if (y.size() != p.size() || y.empty()) return std::nan("");
    double acc = 0;
    for (size_t i = 0; i < y.size(); ++i) {
        const double d = p[i] - static_cast<double>(y[i]);
        acc += d * d;
    }
    return acc / static_cast<double>(y.size());
}

// Expected calibration error over equal-width probability bins: the average
// gap between "the model said 80%" and "80% of those actually won", weighted by
// how often each bin occurs. This is the number that decides whether a
// threshold of 0.8 means anything.
[[nodiscard]] inline double expected_calibration_error(const std::vector<int>& y,
                                                       const std::vector<double>& p,
                                                       int bins = 10) {
    if (y.size() != p.size() || y.empty() || bins < 2) return std::nan("");
    std::vector<double> sum_p(static_cast<size_t>(bins), 0.0);
    std::vector<double> sum_y(static_cast<size_t>(bins), 0.0);
    std::vector<double> n(static_cast<size_t>(bins), 0.0);
    for (size_t i = 0; i < y.size(); ++i) {
        const double q = std::clamp(p[i], 0.0, 1.0);
        auto b = static_cast<size_t>(q * static_cast<double>(bins));
        if (b >= static_cast<size_t>(bins)) b = static_cast<size_t>(bins) - 1;
        sum_p[b] += q;
        sum_y[b] += static_cast<double>(y[i]);
        n[b] += 1.0;
    }
    double ece = 0;
    const double total = static_cast<double>(y.size());
    for (size_t b = 0; b < static_cast<size_t>(bins); ++b) {
        if (n[b] == 0.0) continue;
        ece += (n[b] / total) * std::abs(sum_y[b] / n[b] - sum_p[b] / n[b]);
    }
    return ece;
}

// ---------------------------------------------------------------------------
// Filter economics
// ---------------------------------------------------------------------------

struct SetEconomics {
    int64_t count = 0;
    double win_rate = std::nan("");
    double expectancy = std::nan("");     // mean net return per signal, after costs
    double total_return = std::nan("");   // sum of net returns
    double profit_factor = std::nan("");
    double return_per_candidate = std::nan("");  // total_return / candidates offered
};

[[nodiscard]] inline SetEconomics economics_of(const std::vector<int>& y,
                                               const std::vector<double>& net_return,
                                               const std::vector<size_t>& selected,
                                               size_t candidates_offered) {
    SetEconomics e;
    e.count = static_cast<int64_t>(selected.size());
    if (selected.empty()) {
        // A filter that accepts nothing produces no return. That is a defined,
        // and very bad, outcome — not an undefined one.
        e.win_rate = std::nan("");
        e.expectancy = std::nan("");
        e.total_return = 0.0;
        e.return_per_candidate = 0.0;
        return e;
    }
    double wins = 0, sum = 0, gains = 0, losses = 0;
    for (const size_t i : selected) {
        wins += static_cast<double>(y[i]);
        sum += net_return[i];
        if (net_return[i] >= 0) gains += net_return[i]; else losses -= net_return[i];
    }
    const double n = static_cast<double>(selected.size());
    e.win_rate = wins / n;
    e.expectancy = sum / n;
    e.total_return = sum;
    e.profit_factor = losses > 0 ? gains / losses : std::nan("");
    e.return_per_candidate =
        candidates_offered > 0 ? sum / static_cast<double>(candidates_offered) : std::nan("");
    return e;
}

// The comparison §30 of the spec asks for: every AltStrategy signal versus the
// subset AltSignalQuality accepted, on the same held-out slice.
struct FilterEvaluation {
    double threshold = 0;
    double acceptance_rate = 0;
    SetEconomics accepted;
    SetEconomics baseline;    // every candidate offered
    SetEconomics rejected;    // what the filter declined — its cost of being wrong
    double expectancy_uplift = std::nan("");         // accepted - baseline
    double win_rate_uplift = std::nan("");
    double return_per_candidate_uplift = std::nan("");
    double roc_auc = std::nan("");
    double pr_auc = std::nan("");
    double log_loss = std::nan("");
    double brier = std::nan("");
    double ece = std::nan("");
};

[[nodiscard]] inline FilterEvaluation evaluate_filter(const std::vector<int>& y,
                                                      const std::vector<double>& score,
                                                      const std::vector<double>& net_return,
                                                      double threshold) {
    FilterEvaluation out;
    out.threshold = threshold;
    if (y.empty() || y.size() != score.size() || y.size() != net_return.size()) return out;

    std::vector<size_t> all(y.size()), accepted, rejected;
    std::iota(all.begin(), all.end(), size_t{0});
    for (size_t i = 0; i < y.size(); ++i) {
        (score[i] >= threshold ? accepted : rejected).push_back(i);
    }
    out.acceptance_rate = static_cast<double>(accepted.size()) / static_cast<double>(y.size());
    out.accepted = economics_of(y, net_return, accepted, y.size());
    out.baseline = economics_of(y, net_return, all, y.size());
    out.rejected = economics_of(y, net_return, rejected, y.size());
    if (!accepted.empty()) {
        out.expectancy_uplift = out.accepted.expectancy - out.baseline.expectancy;
        out.win_rate_uplift = out.accepted.win_rate - out.baseline.win_rate;
    }
    out.return_per_candidate_uplift =
        out.accepted.return_per_candidate - out.baseline.return_per_candidate;
    out.roc_auc = roc_auc(y, score);
    out.pr_auc = pr_auc(y, score);
    out.log_loss = log_loss(y, score);
    out.brier = brier_score(y, score);
    out.ece = expected_calibration_error(y, score);
    return out;
}

// Per-bucket stability. A filter that adds 40 bps overall by being brilliant on
// one symbol and harmful on the rest is not a filter, it is an accident, so the
// gates check the share of buckets whose uplift is non-negative.
struct StabilityReport {
    int64_t buckets_evaluated = 0;    // buckets with at least min_samples signals
    int64_t buckets_non_negative = 0;
    double non_negative_share = std::nan("");
    double worst_uplift = std::nan("");
    std::string worst_bucket;
};

[[nodiscard]] inline StabilityReport stability_by_bucket(
    const std::vector<int>& y, const std::vector<double>& score,
    const std::vector<double>& net_return, const std::vector<std::string>& bucket,
    double threshold, int64_t min_samples) {
    StabilityReport out;
    std::map<std::string, std::vector<size_t>> groups;
    for (size_t i = 0; i < bucket.size(); ++i) groups[bucket[i]].push_back(i);
    for (const auto& [name, idx] : groups) {
        if (static_cast<int64_t>(idx.size()) < min_samples) continue;
        std::vector<int> gy;
        std::vector<double> gs, gr;
        gy.reserve(idx.size());
        gs.reserve(idx.size());
        gr.reserve(idx.size());
        for (const size_t i : idx) {
            gy.push_back(y[i]);
            gs.push_back(score[i]);
            gr.push_back(net_return[i]);
        }
        const auto ev = evaluate_filter(gy, gs, gr, threshold);
        const double uplift = ev.return_per_candidate_uplift;
        if (!std::isfinite(uplift)) continue;
        ++out.buckets_evaluated;
        if (uplift >= 0.0) ++out.buckets_non_negative;
        if (!std::isfinite(out.worst_uplift) || uplift < out.worst_uplift) {
            out.worst_uplift = uplift;
            out.worst_bucket = name;
        }
    }
    if (out.buckets_evaluated > 0) {
        out.non_negative_share = static_cast<double>(out.buckets_non_negative) /
                                 static_cast<double>(out.buckets_evaluated);
    }
    return out;
}

} // namespace altsignalquality::domain
