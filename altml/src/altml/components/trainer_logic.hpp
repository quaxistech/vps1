#pragma once

// Pure, unit-tested logic for the native in-process trainer. No I/O here.
//
// The temporal-split and economic-evaluation semantics are a faithful port of
// the retired Python protocol (tr-1.4.0): timestamp-grouped expanding
// walk-forward, purge/embargo around every boundary, conviction threshold
// calibrated on a prior slice only, one non-overlapping position, directional
// long/short with explicit round-trip fees and slippage.

#include <algorithm>
#include <cmath>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace altml::training {

// ---------------------------------------------------------------- PG parsing

// Round-trip cost derived from one row of altml.fee_models.
//
// The same table the label builder reads, so `future_return_after_fees_*` and
// the economic gate price a trade identically. Both legs are charged: a round
// turn pays the taker fee twice and crosses the spread twice.
//
// `taker_fee` is a fraction (0.0006 = 6 bps); `slippage_estimate_bps` is
// already in basis points. Returns {fee_bps, slippage_bps}.
struct RoundTripCost {
    double fee_bps = 0;
    double slippage_bps = 0;
};

[[nodiscard]] inline RoundTripCost round_trip_from_fee_model(double taker_fee,
                                                             double slippage_estimate_bps) {
    return {2.0 * taker_fee * 10'000.0, 2.0 * slippage_estimate_bps};
}

// Parse a PostgreSQL float array literal: {1.5,-2,NULL,3e-4}. NULL -> NaN.
// How many training rows a deployment may hold at once.
//
// Two ceilings govern this and they are not the same quantity: `max_rows` is a
// policy about dataset size, `max_matrix_mib` is a property of the host. The
// sampling loop used to coarsen against the first while the gate after it
// refused against the second, so a dataset could satisfy the loop and still be
// rejected with no way back. As the archive grew past the memory bound the
// trainer stopped running altogether — measured here, no completed cycle since
// 08-09 against ~955k ingested feature vectors.
//
// The factor of 4*4 is four bytes per float and the working copies LightGBM
// holds beside the matrix it is given.
[[nodiscard]] inline int64_t memory_row_cap(int64_t max_matrix_mib, int32_t n_features) noexcept {
    if (n_features <= 0 || max_matrix_mib <= 0) return 0;
    return (max_matrix_mib * 1024 * 1024) / (4LL * 4LL * static_cast<int64_t>(n_features));
}

// The binding ceiling: whichever of the two is smaller, never below one row.
[[nodiscard]] inline int64_t training_row_cap(int64_t max_rows, int64_t max_matrix_mib,
                                              int32_t n_features) noexcept {
    const int64_t by_memory = memory_row_cap(max_matrix_mib, n_features);
    const int64_t effective = by_memory > 0 ? by_memory : max_rows;
    const int64_t cap = max_rows < effective ? max_rows : effective;
    return cap > 1 ? cap : 1;
}

inline std::vector<float> parse_pg_float_array(std::string_view text) {
    std::vector<float> out;
    if (text.size() < 2 || text.front() != '{' || text.back() != '}') {
        throw std::invalid_argument("not a PostgreSQL array literal");
    }
    const std::string_view body = text.substr(1, text.size() - 2);
    if (body.empty()) return out;
    size_t start = 0;
    while (start <= body.size()) {
        size_t end = body.find(',', start);
        if (end == std::string_view::npos) end = body.size();
        const std::string_view tok = body.substr(start, end - start);
        if (tok == "NULL") {
            out.push_back(std::numeric_limits<float>::quiet_NaN());
        } else {
            out.push_back(std::stof(std::string(tok)));
        }
        if (end == body.size()) break;
        start = end + 1;
    }
    return out;
}

// Parse PostgreSQL hex bytea text ("\x0aff") into bytes.
inline std::vector<uint8_t> parse_pg_hex_bytea(std::string_view text) {
    if (text.size() < 2 || text[0] != '\\' || (text[1] != 'x' && text[1] != 'X')) {
        throw std::invalid_argument("not a hex bytea literal");
    }
    const std::string_view hex = text.substr(2);
    if (hex.size() % 2 != 0) throw std::invalid_argument("odd-length bytea hex");
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        throw std::invalid_argument("invalid bytea hex digit");
    };
    std::vector<uint8_t> out(hex.size() / 2);
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<uint8_t>((nibble(hex[2 * i]) << 4) | nibble(hex[2 * i + 1]));
    }
    return out;
}

// Build a PostgreSQL float array literal ("{1.5,-2,NaN}") from wire values,
// the inverse of parse_pg_float_array; used to archive a live vector as-is.
template <typename FloatRange>
inline std::string pg_float_array_literal(const FloatRange& values) {
    std::string out = "{";
    bool first = true;
    char buf[32];
    for (const float v : values) {
        if (!first) out += ',';
        first = false;
        std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(v));
        out += buf;
    }
    out += '}';
    return out;
}

// Build a PostgreSQL text array literal ("{a,b}") with every element quoted and
// escaped. Quoting unconditionally keeps elements that contain a comma, a brace
// or a backslash from silently changing arity once they come back out.
inline std::string pg_text_array_literal(const std::vector<std::string>& values) {
    std::string out = "{";
    bool first = true;
    for (const auto& v : values) {
        if (!first) out += ',';
        first = false;
        out += '"';
        for (const char c : v) {
            if (c == '"' || c == '\\') out += '\\';
            out += c;
        }
        out += '"';
    }
    out += '}';
    return out;
}

// Split a PostgreSQL text array literal back into elements.
inline std::vector<std::string> parse_pg_text_array(std::string_view lit) {
    std::vector<std::string> out;
    if (lit.size() < 2 || lit.front() != '{' || lit.back() != '}') return out;
    lit.remove_prefix(1);
    lit.remove_suffix(1);
    std::string cur;
    bool in_quotes = false, has_element = false, escaped = false;
    for (const char c : lit) {
        if (escaped) { cur += c; escaped = false; has_element = true; continue; }
        if (c == '\\') { escaped = true; continue; }
        if (c == '"') { in_quotes = !in_quotes; has_element = true; continue; }
        if (c == ',' && !in_quotes) { out.push_back(cur); cur.clear(); has_element = false; continue; }
        cur += c;
        has_element = true;
    }
    if (has_element || !cur.empty()) out.push_back(cur);
    return out;
}

// Build a PostgreSQL hex bytea literal ("\x0aff") from raw bytes, the inverse
// of parse_pg_hex_bytea.
inline std::string pg_bytea_hex_literal(std::string_view bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out = "\\x";
    out.reserve(2 + bytes.size() * 2);
    for (const unsigned char b : bytes) {
        out += kHex[b >> 4];
        out += kHex[b & 0x0F];
    }
    return out;
}

// Little-endian bit i of the validity bitmap (matches the wire/DB contract).
inline bool bitmap_bit(const std::vector<uint8_t>& bitmap, size_t i) {
    return (i / 8) < bitmap.size() && (((bitmap[i / 8] >> (i % 8)) & 1U) != 0U);
}

// ------------------------------------------------------------ temporal splits

struct FoldIdx {
    std::vector<int64_t> train;
    std::vector<int64_t> valid;
    std::vector<int64_t> test;
};

struct FinalSplitIdx {
    std::vector<int64_t> train;  // tuning fit (before validation, embargoed)
    std::vector<int64_t> valid;  // early-stopping / threshold calibration
    std::vector<int64_t> fit;    // full refit rows (everything before test, embargoed)
    std::vector<int64_t> test;   // held-out final slice, touched exactly once
};

inline std::vector<int64_t> sorted_unique(const std::vector<int64_t>& times) {
    std::vector<int64_t> unique = times;
    std::sort(unique.begin(), unique.end());
    unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
    return unique;
}

// Boundary positions: n_folds + 4 evenly spaced edges over the distinct
// timestamps, reserving one segment after all fold tests for the final
// held-out test (identical to the retired Python protocol).
inline std::vector<size_t> split_edges(size_t n_unique, size_t n_folds) {
    if (n_folds < 1 || n_unique < n_folds + 4) {
        throw std::invalid_argument("not enough distinct timestamps for walk-forward folds");
    }
    const size_t k = n_folds + 4;
    std::vector<size_t> edges(k);
    for (size_t i = 0; i < k; ++i) {
        // linspace(0, n_unique, k) with integer truncation, like numpy's
        // dtype=int conversion of evenly spaced doubles.
        edges[i] = static_cast<size_t>(
            static_cast<double>(i) * static_cast<double>(n_unique) /
            static_cast<double>(k - 1));
    }
    for (size_t i = 1; i < k; ++i) {
        if (edges[i] <= edges[i - 1]) {
            throw std::invalid_argument("walk-forward timestamp boundaries collapsed");
        }
    }
    return edges;
}

inline std::vector<int64_t> rows_in(const std::vector<int64_t>& times, int64_t lo_inclusive,
                                    int64_t hi_exclusive) {
    std::vector<int64_t> out;
    for (size_t i = 0; i < times.size(); ++i) {
        if (times[i] >= lo_inclusive && times[i] < hi_exclusive) {
            out.push_back(static_cast<int64_t>(i));
        }
    }
    return out;
}

// Expanding timestamp-grouped folds with a purge/embargo gap before every
// validation and test boundary. Row indices refer to the original arrays.
inline std::vector<FoldIdx> build_walk_forward_folds(const std::vector<int64_t>& times_ms,
                                                     size_t n_folds, int64_t embargo_ms) {
    if (times_ms.empty()) throw std::invalid_argument("empty timestamps");
    const auto unique = sorted_unique(times_ms);
    const auto edges = split_edges(unique.size(), n_folds);
    constexpr int64_t kFar = std::numeric_limits<int64_t>::max();
    std::vector<FoldIdx> folds;
    for (size_t k = 0; k < n_folds; ++k) {
        const int64_t valid_start = unique[edges[k + 1]];
        const int64_t test_start = unique[edges[k + 2]];
        const int64_t test_stop = edges[k + 3] < unique.size() ? unique[edges[k + 3]] : kFar;
        FoldIdx fold;
        fold.train = rows_in(times_ms, std::numeric_limits<int64_t>::min(),
                             valid_start - embargo_ms);
        fold.valid = rows_in(times_ms, valid_start, test_start - embargo_ms);
        fold.test = rows_in(times_ms, test_start, test_stop);
        if (fold.train.empty() || fold.valid.empty() || fold.test.empty()) {
            throw std::invalid_argument("fold " + std::to_string(k) +
                                        " empty after purge/embargo");
        }
        folds.push_back(std::move(fold));
    }
    return folds;
}

inline FinalSplitIdx build_final_split(const std::vector<int64_t>& times_ms, size_t n_folds,
                                       int64_t embargo_ms, double validation_fraction) {
    const auto unique = sorted_unique(times_ms);
    const auto edges = split_edges(unique.size(), n_folds);
    const int64_t test_start = unique[edges[n_folds + 2]];
    std::vector<int64_t> safe_pretest;
    for (const int64_t u : unique) {
        if (u < test_start - embargo_ms) safe_pretest.push_back(u);
    }
    if (safe_pretest.size() < 3) {
        throw std::invalid_argument("not enough pre-test timestamps after embargo");
    }
    size_t pos = static_cast<size_t>(static_cast<double>(safe_pretest.size()) *
                                     (1.0 - validation_fraction));
    pos = std::min(std::max<size_t>(pos, 1), safe_pretest.size() - 1);
    const int64_t valid_start = safe_pretest[pos];
    FinalSplitIdx split;
    constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
    constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
    split.train = rows_in(times_ms, kMin, valid_start - embargo_ms);
    split.valid = rows_in(times_ms, valid_start, test_start - embargo_ms);
    split.fit = rows_in(times_ms, kMin, test_start - embargo_ms);
    split.test = rows_in(times_ms, test_start, kMax);
    if (split.train.empty() || split.valid.empty() || split.fit.empty() || split.test.empty()) {
        throw std::invalid_argument("final split empty after purge/embargo");
    }
    return split;
}

// --------------------------------------------------------------- metrics

// ROC-AUC by rank statistic (ties get average rank). y: 0/1 only.
inline double roc_auc(const std::vector<int>& y, const std::vector<double>& p) {
    if (y.size() != p.size() || y.empty()) return std::nan("");
    std::vector<size_t> order(y.size());
    std::iota(order.begin(), order.end(), size_t{0});
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return p[a] < p[b]; });
    double rank_sum_pos = 0;
    size_t n_pos = 0, n_neg = 0;
    size_t i = 0;
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

inline double log_loss(const std::vector<int>& y, const std::vector<double>& p) {
    if (y.size() != p.size() || y.empty()) return std::nan("");
    double acc = 0;
    for (size_t i = 0; i < y.size(); ++i) {
        const double q = std::clamp(p[i], 1e-15, 1.0 - 1e-15);
        acc -= y[i] == 1 ? std::log(q) : std::log(1.0 - q);
    }
    return acc / static_cast<double>(y.size());
}

// Fixed conviction threshold calibrated on a PRIOR (non-test) slice: the
// `quantile` of |p - 0.5| (linear interpolation, like numpy.quantile).
inline double conviction_threshold(const std::vector<double>& p, double quantile) {
    if (p.empty()) throw std::invalid_argument("empty calibration predictions");
    if (quantile < 0.0 || quantile > 1.0) throw std::invalid_argument("bad quantile");
    std::vector<double> conviction(p.size());
    for (size_t i = 0; i < p.size(); ++i) {
        if (!std::isfinite(p[i]) || p[i] < 0.0 || p[i] > 1.0) {
            throw std::invalid_argument("calibration prediction outside [0, 1]");
        }
        conviction[i] = std::abs(p[i] - 0.5);
    }
    std::sort(conviction.begin(), conviction.end());
    const double pos = quantile * static_cast<double>(conviction.size() - 1);
    const size_t lo = static_cast<size_t>(pos);
    const size_t hi = std::min(lo + 1, conviction.size() - 1);
    const double frac = pos - static_cast<double>(lo);
    return conviction[lo] * (1.0 - frac) + conviction[hi] * frac;
}

// One trade per holding window: at each timestamp the highest-conviction row
// wins (symbol id, then row index break ties deterministically); the next
// trade may start only after `holding_ms`.
inline std::vector<int64_t> select_non_overlapping(const std::vector<double>& p,
                                                   const std::vector<int64_t>& times_ms,
                                                   const std::vector<int32_t>& symbol_ids,
                                                   double threshold, int64_t holding_ms) {
    if (p.size() != times_ms.size() || p.size() != symbol_ids.size()) {
        throw std::invalid_argument("prediction/time/symbol lengths differ");
    }
    if (!(threshold >= 0.0 && threshold <= 0.5)) {
        throw std::invalid_argument("conviction threshold outside [0, 0.5]");
    }
    std::vector<size_t> eligible;
    for (size_t i = 0; i < p.size(); ++i) {
        if (std::abs(p[i] - 0.5) >= threshold) eligible.push_back(i);
    }
    std::sort(eligible.begin(), eligible.end(), [&](size_t a, size_t b) {
        if (times_ms[a] != times_ms[b]) return times_ms[a] < times_ms[b];
        const double ca = std::abs(p[a] - 0.5), cb = std::abs(p[b] - 0.5);
        if (ca != cb) return ca > cb; // best conviction first inside a stamp
        if (symbol_ids[a] != symbol_ids[b]) return symbol_ids[a] < symbol_ids[b];
        return a < b;
    });
    std::vector<int64_t> selected;
    int64_t next_free = std::numeric_limits<int64_t>::min();
    size_t i = 0;
    while (i < eligible.size()) {
        const int64_t stamp = times_ms[eligible[i]];
        if (stamp >= next_free) {
            selected.push_back(static_cast<int64_t>(eligible[i])); // best of this stamp
            next_free = stamp + holding_ms;
        }
        while (i < eligible.size() && times_ms[eligible[i]] == stamp) ++i;
    }
    return selected;
}

struct EconomicResult {
    int64_t trades = 0;
    double expectancy_after_costs = std::nan("");  // mean per-trade net return
    double profit_factor = std::nan("");
    double hit_rate = std::nan("");
    double max_drawdown = std::nan("");
    double total_return = std::nan("");
    double long_share = std::nan("");
};

// Directional long/short evaluation with symmetric explicit costs on a linear
// (USDT-M) contract: short PnL is the negative simple price return. Equity is
// multiplicative, one position at a time, full exposure.
inline EconomicResult evaluate_directional(const std::vector<int64_t>& selected,
                                           const std::vector<double>& p,
                                           const std::vector<double>& raw_log_returns,
                                           double round_trip_fee_bps,
                                           double round_trip_slippage_bps) {
    EconomicResult out;
    out.trades = static_cast<int64_t>(selected.size());
    if (selected.empty()) return out;
    const double costs = (round_trip_fee_bps + round_trip_slippage_bps) / 10'000.0;
    double equity = 1.0, peak = 1.0, max_dd = 0.0, gains = 0.0, losses = 0.0;
    double sum_net = 0.0;
    int64_t wins = 0, longs = 0;
    for (const int64_t idx : selected) {
        const auto i = static_cast<size_t>(idx);
        const double direction = p[i] >= 0.5 ? 1.0 : -1.0;
        if (direction > 0) ++longs;
        const double net = direction * std::expm1(raw_log_returns[i]) - costs;
        sum_net += net;
        if (net > 0) ++wins;
        const double pnl = equity * net;
        equity += pnl;
        if (pnl >= 0) gains += pnl; else losses -= pnl;
        peak = std::max(peak, equity);
        max_dd = std::max(max_dd, (peak - equity) / peak);
    }
    const double n = static_cast<double>(selected.size());
    out.expectancy_after_costs = sum_net / n;
    out.profit_factor = losses > 0 ? gains / losses : std::nan("");
    out.hit_rate = static_cast<double>(wins) / n;
    out.max_drawdown = max_dd;
    out.total_return = equity - 1.0;
    out.long_share = static_cast<double>(longs) / n;
    return out;
}

// ---------------------------------------------------------- T0 alignment

// One additional observation source: a venue and a grid, joined as of the
// target bar's close.
struct AlignedSource {
    int exchange_id = 1;
    std::string timeframe = "1m";
    int span_minutes = 1;       // how long a bar on this grid lasts
    int64_t span_milliseconds = 0; // explicit sub-minute window; never rounded to1m
    // 0 = no lower bound at all: the newest bar that has closed by T0 is
    // taken however old it is. It does NOT mean 'only the bar closing
    // exactly at T0' -- aligned_source_sql() emits the lower bound solely
    // when this is positive, and the in-memory rule matches. Every
    // configured source sets it, so an unbounded source is a decision.
    int freshness_minutes = 0;
    // Which feature set this source reads. talib-core is 995 indicator columns
    // derived from candles alone; reference_lite carries what candles cannot
    // express -- spread, book depth and imbalance, microprice, trade flow, CVD,
    // funding, open interest, mark/index deviation. Both are archived for every
    // venue, and pinning the VERSION here is what keeps a model reproducible
    // after the set moves on: v3 and v4 of reference_lite are both frozen in the
    // archive and have different columns at the same positions.
    std::string feature_set = "talib-core";
    int feature_set_version = 1;
    // Every frozen version of this set whose column list is byte-identical to the
    // named one -- the named version included. Rows are selected on THIS, not on
    // the single version, and not on schema_hash.
    //
    // Two facts force it. reference_lite_micro carries four frozen versions with
    // identical columns (the number was bumped alongside a different set), so
    // 374k rows of history sit under v1 and recent ones under v4 while every one
    // has the same layout: matching one version hides the history. And
    // schema_hash cannot stand in for the layout either -- AltTech leaves it
    // EMPTY on the wire for reference_lite and cross_venue_core, so those rows
    // carry no hash at all in the archive. The equivalent-version set is the one
    // key that is present on every row and still refuses to mix layouts:
    // reference_lite v3 and v4 differ in columns, so they never share a set.
    std::vector<int> equivalent_versions;
    // The columns taken from this source, and where they sit in its vector.
    // Filled by whoever resolves the set's layout; both sides resolve it the
    // same way or the served block would be a permutation of the trained one.
    std::vector<std::string> projection;
    std::vector<size_t> positions;
    size_t published_width = 0;
    std::string engine_version; // optional explicit producer-method pin in the model recipe
    // Set when this source is the SAME observation for every target, rather
    // than one observation per instrument.
    //
    // The macro block is that: USDCNH does not depend on which crypto pair is
    // being predicted, so it is archived once under a fixed instrument key and
    // joined to every row. Without this the same numbers would have to be
    // duplicated across all 27 instruments -- eleven million rows carrying the
    // information of four hundred thousand.
    std::optional<int64_t> pinned_instrument_id;
    std::string alias = "s0";
};

[[nodiscard]] inline int64_t source_span_ms(const AlignedSource& src) {
    return src.span_milliseconds > 0 ? src.span_milliseconds :
           static_cast<int64_t>(src.span_minutes) * 60'000;
}

[[nodiscard]] inline std::string source_span_interval(const AlignedSource& src) {
    return src.span_milliseconds > 0 ? std::to_string(src.span_milliseconds) + " milliseconds" :
                                      std::to_string(src.span_minutes) + " minutes";
}

// Comma-separated version list for the SQL ARRAY literal. Falls back to the
// named version alone, so a source is never silently unfiltered.
[[nodiscard]] inline std::string version_list(const AlignedSource& src) {
    std::string out;
    for (const int v : src.equivalent_versions) {
        if (!out.empty()) out += ',';
        out += std::to_string(v);
    }
    if (out.empty()) out = std::to_string(src.feature_set_version);
    return out;
}

// SQL for one point-in-time source join.
//
// The rule this encodes, and the reason it is a LATERAL rather than an equality
// join: a bar that OPENS at O on a grid of span S is not observable until it
// CLOSES, at O + S. The target's own 1m bar opening at `bar_open_time` closes
// one minute later, and that instant is T0 -- the moment a decision could have
// been made. So a source bar qualifies only when
//
//     O + S  <=  bar_open_time + 1 minute
//
// and the newest such bar is the one a live system would have had. Joining on
// `bar_open_time = bar_open_time` instead would hand the model a 1h bar that
// had not finished forming, which is the most expensive kind of leakage: it
// looks like a feature and behaves like the answer.
//
// `freshness_minutes` bounds how stale an observation may be before it is
// treated as absent. Absent means NULL means NaN -- never a substituted value
// from another venue, another grid, or a default.
[[nodiscard]] inline std::string aligned_source_sql(const AlignedSource& src) {
    const std::string a = src.alias;
    std::string sql =
        "LEFT JOIN LATERAL (SELECT " + a + "_i.values AS values FROM altml.feature_vectors " + a + "_i "
        "WHERE " + a + "_i.exchange_id = " + std::to_string(src.exchange_id) + " "
        // A pinned source is the same observation for every target; an
        // unpinned one is the target's own instrument.
        "AND " + a + "_i.instrument_id = " +
            (src.pinned_instrument_id.has_value()
                 ? std::to_string(*src.pinned_instrument_id)
                 : std::string("f.instrument_id")) + " "
        "AND " + a + "_i.timeframe = '" + src.timeframe + "' "
        "AND " + a + "_i.feature_set_name = '" + src.feature_set + "' "
        // Every version that has this exact layout. The name alone would blend
        // two different column lists into one column and the blend would look
        // like data; a single version would drop history published under an
        // older number with the very same columns.
        "AND " + a + "_i.feature_set_version = ANY(ARRAY[" + version_list(src) + "]) "
        // The bound is written with the interval on the TARGET side, never on
        // the indexed column.
        //
        // `bar_open_time + interval <= X` is not sargable: the planner cannot
        // use an index on a column it has to compute first, so the time bound
        // dropped into a Filter and every LATERAL scanned the whole instrument
        // in every partition. Measured on VPS1 over 24 hours of one instrument
        // and one source, the two forms are 5493 ms and 37 ms -- a factor of
        // 147. With 54 362 candidate rows and six sources the training query
        // could not finish at all: `training_cycle_failed: canceling statement
        // due to statement timeout`, which is why the model registry was empty.
        //
        // `a + i <= b` and `a <= b - i` are the same set of rows.
        "AND " + a + "_i.bar_open_time <= f.bar_open_time + interval '1 minute'"
        " - interval '" + source_span_interval(src) + "' ";
    if (!src.engine_version.empty()) sql += "AND " + a + "_i.engine_version = '" + src.engine_version + "' ";
    if (src.freshness_minutes > 0) {
        sql += "AND " + a + "_i.bar_open_time >= f.bar_open_time + interval '1 minute'"
               " - interval '" + source_span_interval(src) + "'"
               " - interval '" + std::to_string(src.freshness_minutes) + " minutes' ";
    }
    sql += "ORDER BY " + a + "_i.bar_open_time DESC LIMIT 1) " + a + " ON true ";
    return sql;
}

// The point-in-time form used by the econ trainer: values AND validity bitmap,
// and an observation is admitted only if AltTech had COMPUTED it by the time
// the target's own vector was computed. A bar that had closed but was not yet
// computed -- the 15m grid is computed a median three minutes after its close
// on VPS1 -- is not something a live system could have read, and neither is a
// row backfilled hours later.
[[nodiscard]] inline std::string aligned_source_sql_pit(const AlignedSource& src) {
    const std::string a = src.alias;
    std::string sql =
        "LEFT JOIN LATERAL (SELECT " + a + "_i.values AS values, " + a +
        "_i.validity_bitmap AS bmp FROM altml.econ_feature_vectors " + a + "_i "
        "WHERE " + a + "_i.exchange_id = " + std::to_string(src.exchange_id) + " "
        "AND " + a + "_i.instrument_id = " +
            (src.pinned_instrument_id.has_value() ? std::to_string(*src.pinned_instrument_id)
                                                  : std::string("f.instrument_id")) + " "
        "AND " + a + "_i.timeframe = '" + src.timeframe + "' "
        "AND " + a + "_i.feature_set_name = '" + src.feature_set + "' "
        "AND " + a + "_i.feature_set_version = ANY(ARRAY[" + version_list(src) + "]) "
        "AND " + a + "_i.quality_status = 'VALID' "
        "AND " + a + "_i.bar_open_time <= f.bar_open_time + interval '1 minute'"
        " - interval '" + source_span_interval(src) + "' ";
    if (src.published_width > 0) {
        sql += "AND cardinality(" + a + "_i.values) = " + std::to_string(src.published_width) + " ";
    }
    if (!src.engine_version.empty()) sql += "AND " + a + "_i.engine_version = '" + src.engine_version + "' ";
    if (src.freshness_minutes > 0) {
        sql += "AND " + a + "_i.bar_open_time >= f.bar_open_time + interval '1 minute'"
               " - interval '" + source_span_interval(src) + "'"
               " - interval '" + std::to_string(src.freshness_minutes) + " minutes' ";
    }
    // Receipt is an independent dependency clock: a value computed earlier
    // but delivered/backfilled after the target was received was unavailable
    // to that decision. Never reconstruct live availability from event time.
    sql += "AND " + a + "_i.computed_at <= f.computed_at "
           "AND " + a + "_i.received_at <= f.received_at "
           "ORDER BY " + a + "_i.bar_open_time DESC, " + a + "_i.computed_at DESC, " +
           a + "_i.bar_revision DESC, " + a + "_i.received_at DESC LIMIT 1) " +
           a + " ON true ";
    return sql;
}

// Minutes a bar of the named grid lasts. Unknown grids are rejected rather
// than defaulted -- a wrong span silently shifts the whole alignment.
[[nodiscard]] inline std::optional<int> grid_span_minutes(std::string_view tf) {
    if (tf == "1m") return 1;
    if (tf == "5m") return 5;
    if (tf == "15m") return 15;
    if (tf == "30m") return 30;
    if (tf == "1h") return 60;
    if (tf == "4h") return 240;
    if (tf == "1d") return 1440;
    return std::nullopt;
}

// Parse one "exchange_id:timeframe:freshness_minutes" spec.
//
// Shared by the trainer, which turns specs into SQL, and by inference, which
// turns the SAME specs -- read back from the model's own record, never from the
// live config -- into slots it fills from the stream. One parser is the point:
// if the two sides could disagree about what a spec means, the model would be
// served an input shaped differently from the one it learned on, and nothing
// downstream would be able to tell.
[[nodiscard]] inline std::optional<AlignedSource> parse_aligned_source(const std::string& spec,
                                                                      size_t index) {
    // "venue:grid:freshness" or "venue:grid:freshness:set:set_version".
    // The short form means talib-core:1, which is what every source was before
    // sets other than talib-core could be read.
    std::vector<std::string_view> parts;
    size_t start = 0;
    for (;;) {
        const auto colon = spec.find(':', start);
        parts.emplace_back(std::string_view(spec).substr(start,
            colon == std::string::npos ? colon : colon - start));
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    const auto matched = parts.size();
    if (matched != 3 && matched != 5 && matched != 6 && matched != 7) return std::nullopt;
    const auto number = [](std::string_view text, auto& value) {
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        return !text.empty() && parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
    };
    int venue = 0, fresh = 0, set_version = 1;
    int64_t pinned = -1;
    if (!number(parts[0], venue) || !number(parts[2], fresh)) return std::nullopt;
    if (matched >= 5 && !number(parts[4], set_version)) return std::nullopt;
    if (matched >= 6 && !(matched == 7 && parts[5] == "-") && !number(parts[5], pinned)) return std::nullopt;
    const std::string tf(parts[1]);
    const auto span = grid_span_minutes(tf);
    int64_t fast_span_ms = 0;
    if (tf == "1s") fast_span_ms = 1'000;
    else if (tf == "5s") fast_span_ms = 5'000;
    else if (tf == "15s") fast_span_ms = 15'000;
    else if (tf == "30s") fast_span_ms = 30'000;
    // venue 0 is the cross-venue consensus: it belongs to no exchange, is keyed
    // by canonical pair -- the same instrument_id the archive uses for every
    // venue -- and is archived with exchange_id 0. It is a real observation, not
    // an unknown venue, so it is admitted here and nowhere else.
    if ((!span && fast_span_ms == 0) || venue < 0 || venue > 4 || fresh < 0) return std::nullopt;
    AlignedSource src;
    src.exchange_id = venue;
    src.timeframe = tf;
    src.span_minutes = span.value_or(0);
    src.span_milliseconds = fast_span_ms;
    src.freshness_minutes = fresh;
    if (matched >= 5) {
        if (set_version < 1) return std::nullopt;
        src.feature_set = parts[3];
        src.feature_set_version = set_version;
        if (src.feature_set.empty() || src.feature_set.size() > 63 ||
            !std::all_of(src.feature_set.begin(), src.feature_set.end(), [](unsigned char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || c == '_' || c == '-';
            })) return std::nullopt;
    }
    if (matched >= 6 && parts[5] != "-") {
        if (pinned < 0) return std::nullopt;
        src.pinned_instrument_id = static_cast<int64_t>(pinned);
    }
    if (matched == 7) {
        src.engine_version = parts[6];
        if (src.engine_version.empty() || src.engine_version.size() > 80 ||
            !std::all_of(src.engine_version.begin(), src.engine_version.end(), [](unsigned char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
            })) return std::nullopt;
    }
    if (fast_span_ms > 0 && src.feature_set != "bitget_micro" &&
        src.feature_set != "reference_lite_micro" && src.feature_set != "book_ofi_receipt") return std::nullopt;
    if (src.feature_set == "book_ofi_receipt" && (venue < 2 || set_version != 1)) return std::nullopt;
    // The per-venue indicator set has no cross-venue edition, and the consensus
    // set has no per-venue edition. Either mix-up would join a set that does not
    // exist and yield a column of NULLs that looks like a quiet market.
    //
    // Venue 0 now covers two different things -- the cross-venue consensus,
    // which is per pair, and pinned sources such as the macro block, which are
    // per nothing -- so the rule is written against the pin rather than the
    // venue alone.
    if (src.pinned_instrument_id.has_value()) {
        if (venue != 0) return std::nullopt;   // a pinned source belongs to no venue
    } else if ((venue == 0) != (src.feature_set == "cross_venue_core")) {
        return std::nullopt;
    }
    src.alias = "s" + std::to_string(index);
    return src;
}

// Render a source back to its spec, so what a model was trained with can be
// stored and read back as one string.
[[nodiscard]] inline std::string aligned_source_spec(const AlignedSource& src) {
    std::string out = std::to_string(src.exchange_id) + ":" + src.timeframe + ":" +
                      std::to_string(src.freshness_minutes) + ":" + src.feature_set + ":" +
                      std::to_string(src.feature_set_version);
    // The pin is part of the recipe: a model trained against a pinned source
    // must be served from the same one, or inference would join per-instrument
    // rows where training joined a single global series.
    if (src.pinned_instrument_id.has_value()) {
        out += ":" + std::to_string(*src.pinned_instrument_id);
    } else if (!src.engine_version.empty()) {
        out += ":-";
    }
    if (!src.engine_version.empty()) out += ":" + src.engine_version;
    return out;
}

// Column names for the aligned block, in matrix order: source-major, then
// projection. Provenance in the name itself -- venue, grid, feature.
[[nodiscard]] inline std::vector<std::string> aligned_feature_names(
    const std::vector<AlignedSource>& sources) {
    std::vector<std::string> out;
    for (const auto& src : sources) {
        for (const auto& name : src.projection) {
            out.push_back("v" + std::to_string(src.exchange_id) + "_" + src.timeframe + "_" +
                          src.feature_set + "_" + name);
        }
    }
    return out;
}

// Resolve which columns a source contributes, given the layout of the set it
// reads. Both the trainer and inference call this with the same layout, read
// from the same registry by name and version, so the served block is the
// trained block rather than a permutation of it.
//
// talib-core is projected -- 995 columns per source across many venue/grid
// combinations is tens of thousands of columns against far fewer independent
// observations. Any other set is taken whole: those sets are small, and their
// columns (spread, book depth, imbalance, microprice, CVD, flow, funding, open
// interest, mark and index deviation) are exactly what candles cannot express,
// so there is nothing to project away.
[[nodiscard]] inline bool resolve_source_projection(AlignedSource& src,
                                                    const std::vector<std::string>& layout,
                                                    const std::vector<std::string>& core_projection,
                                                    std::string_view core_set_name) {
    src.projection.clear();
    src.positions.clear();
    if (layout.empty()) return false;
    if (src.feature_set == core_set_name) {
        for (const auto& want : core_projection) {
            const auto it = std::find(layout.begin(), layout.end(), want);
            if (it == layout.end()) return false;  // an unknown name shrinks the
                                                   // space silently; refuse instead
            src.projection.push_back(want);
            src.positions.push_back(static_cast<size_t>(it - layout.begin()));
        }
    } else {
        for (size_t i = 0; i < layout.size(); ++i) {
            src.projection.push_back(layout[i]);
            src.positions.push_back(i);
        }
    }
    return !src.projection.empty();
}

// Total width of the aligned block. Sources no longer contribute equally: a
// talib-core source contributes its projection, a reference_lite source its
// whole (small) set.
[[nodiscard]] inline size_t aligned_block_width(const std::vector<AlignedSource>& sources) {
    size_t n = 0;
    for (const auto& src : sources) n += src.projection.size();
    return n;
}

// Whether a source observation is usable at T0, the instant the target's 1m bar
// closes. The bar must have CLOSED by T0, and must not be older than its
// freshness bound. This is the same rule aligned_source_sql() encodes in SQL;
// inference applies it in memory to the vectors it has actually received.
[[nodiscard]] inline bool aligned_observation_usable(const AlignedSource& src,
                                                     int64_t source_bar_open_ms,
                                                     int64_t t0_ms) noexcept {
    constexpr int64_t kMinuteMs = 60'000;
    const auto span_ms = source_span_ms(src);
    if (span_ms <= 0 || source_bar_open_ms < 0 ||
        source_bar_open_ms > std::numeric_limits<int64_t>::max() - span_ms) return false;
    const int64_t closes_at = source_bar_open_ms + span_ms;
    if (closes_at > t0_ms) return false;
    if (src.freshness_minutes > 0 &&
        closes_at < t0_ms - static_cast<int64_t>(src.freshness_minutes) * kMinuteMs) {
        return false;
    }
    return true;
}

// The online half of aligned_source_sql_pit. Source computation and receipt
// must both precede the corresponding target clocks; neither SELECT time nor
// a republished envelope can make a historical dependency available sooner.
[[nodiscard]] inline bool aligned_observation_usable_pit(const AlignedSource& src,
    int64_t source_bar_open_ms, int64_t source_computed_ms, int64_t source_received_ms,
    int64_t t0_ms, int64_t target_computed_ms, int64_t target_received_ms) noexcept {
    return source_bar_open_ms >= 0 && source_computed_ms > 0 && source_received_ms > 0 &&
           target_computed_ms > 0 && target_received_ms > 0 &&
           source_computed_ms <= target_computed_ms && source_received_ms <= target_received_ms &&
           aligned_observation_usable(src, source_bar_open_ms, t0_ms);
}

// -------------------------------------------------- fold-local selection

// Why a feature was excluded. Kept as an enum so the reason can be stored and
// audited rather than reconstructed from a threshold later.
enum class RejectReason { kept, all_missing, constant, extreme_missingness, duplicate };

struct SelectionResult {
    std::vector<size_t> selected;            // column indices, ascending
    std::vector<RejectReason> reason;        // per column, size == n_features
    size_t n_features = 0;

    [[nodiscard]] size_t rejected() const { return n_features - selected.size(); }
};

[[nodiscard]] inline const char* reject_reason_name(RejectReason r) {
    switch (r) {
        case RejectReason::kept: return "kept";
        case RejectReason::all_missing: return "all_missing";
        case RejectReason::constant: return "constant";
        case RejectReason::extreme_missingness: return "extreme_missingness";
        case RejectReason::duplicate: return "duplicate";
    }
    return "unknown";
}

// The selection contract, versioned so a model can name the rules it was
// trained under. Bump it whenever the criteria below change.
inline constexpr int kSelectionVersion = 1;
inline constexpr double kMaxMissingness = 0.60;   // reject above this share

// Choose features using ONLY the rows handed in.
//
// This is the whole point: the caller passes a single fold's TRAINING row
// indices, never the validation or test rows and never the full dataset. A
// filter fitted on all the data has already seen the future of every fold it
// will later be evaluated on, and the resulting score is a measurement of that
// leak rather than of the model.
//
// The criteria are deliberately few and cheap -- missingness, constancy,
// duplication. Ranking by importance is left to the booster, which does it
// while fitting and cannot leak by doing so.
[[nodiscard]] inline SelectionResult select_features_fold_local(
    const std::vector<float>& X, size_t n_features, const std::vector<int64_t>& train_rows) {
    SelectionResult out;
    out.n_features = n_features;
    out.reason.assign(n_features, RejectReason::kept);
    if (n_features == 0 || train_rows.empty()) return out;

    std::vector<size_t> present(n_features, 0);
    std::vector<double> first_seen(n_features, 0.0);
    std::vector<bool> has_first(n_features, false);
    std::vector<bool> varies(n_features, false);

    for (const int64_t r : train_rows) {
        const size_t base = static_cast<size_t>(r) * n_features;
        for (size_t f = 0; f < n_features; ++f) {
            const float v = X[base + f];
            if (!std::isfinite(v)) continue;
            ++present[f];
            if (!has_first[f]) { first_seen[f] = v; has_first[f] = true; }
            else if (!varies[f] && static_cast<double>(v) != first_seen[f]) varies[f] = true;
        }
    }

    const double n = static_cast<double>(train_rows.size());
    for (size_t f = 0; f < n_features; ++f) {
        if (present[f] == 0) { out.reason[f] = RejectReason::all_missing; continue; }
        if (1.0 - static_cast<double>(present[f]) / n > kMaxMissingness) {
            out.reason[f] = RejectReason::extreme_missingness;
            continue;
        }
        if (!varies[f]) { out.reason[f] = RejectReason::constant; continue; }
        out.selected.push_back(f);
    }

    // Exact duplicates: two columns identical on every training row carry the
    // same information, and keeping both only splits the importance between
    // them. The later column goes; the earlier one is the survivor so the
    // choice is deterministic.
    std::vector<size_t> keep;
    keep.reserve(out.selected.size());
    for (const size_t f : out.selected) {
        bool dup = false;
        for (const size_t g : keep) {
            bool same = true;
            for (const int64_t r : train_rows) {
                const size_t base = static_cast<size_t>(r) * n_features;
                const float a = X[base + f];
                const float b = X[base + g];
                const bool na = !std::isfinite(a), nb = !std::isfinite(b);
                if (na != nb || (!na && a != b)) { same = false; break; }
            }
            if (same) { dup = true; break; }
        }
        if (dup) out.reason[f] = RejectReason::duplicate;
        else keep.push_back(f);
    }
    out.selected.swap(keep);
    return out;
}

} // namespace altml::training
