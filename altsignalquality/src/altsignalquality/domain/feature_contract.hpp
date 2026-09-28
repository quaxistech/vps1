#pragma once

// The versioned feature contract: the ONE ordered list of inputs the quality
// model is trained on and served with.
//
// Three properties make this file the centre of the service:
//
//   1. ORDER IS IDENTITY. Features travel as a positional float vector, so
//      position i means whatever this list says it means. The list is hashed
//      into `feature_schema_hash`, every model records the hash it was trained
//      under, and inference refuses a model whose hash differs — a mislabelled
//      feature is otherwise undetectable downstream.
//   2. ONE ASSEMBLER. Training and inference both call `assemble()`. There is
//      no second code path that could drift, which is what "training features
//      == inference features" actually requires.
//   3. MISSING IS EXPLICIT. An unobserved input becomes NaN with its validity
//      bit cleared, and is counted. It is never zero-filled, mean-filled or
//      carried forward: LightGBM learns a real split for "absent", and the
//      count is persisted with the evaluation so a degraded input shows up in
//      the audit instead of hiding inside a plausible number.
//
// Adding, removing or reordering a member is a NEW schema version
// (`kFeatureSchemaVersion`). Old training rows keep their own version, old
// models keep their own hash, and nothing is silently re-labelled.

#include "altsignalquality/domain/signal_snapshot.hpp"
#include "altsignalquality/domain/vocabulary.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace altsignalquality::domain {

// sq-core:2 — dropped `strategy_confidence`, which AltStrategy assigns from
// the SAME variable as `adaptive_score` (engine/proposal.cpp: both are
// `c.adaptive_score`), so it carried no information and split the model's
// feature budget across a duplicate. Verified on 115 873 archived proposals:
// identical in every row.
// sq-core:5 — `net_edge_bps` and `rr_times_p_target` are absent when
// `edge_estimated` is false, instead of reporting 0.0. The layout is unchanged;
// the MEANING of two positions is, which is what the version identifies. The
// dataset backfill recomputes the archive under the new version, so no history
// is lost.
// sq-core:6 — `cost_to_target_ratio` now reproduces AltStrategy's own cost
// model (engine/cost_model.hpp v2) instead of a superseded formula that charged
// 1.5x the spread and used one blended fee for every entry type. Same layout,
// different value for the same inputs, which is what the version identifies.
inline constexpr std::string_view kFeatureSchemaVersion = "sq-core:6";

enum class FeatureKind : uint8_t { numeric, binary, ordinal, cyclic };

enum class FeatureSource : uint8_t {
    proposal,           // the candidate TradeSignalProposal itself
    derived,            // arithmetic over proposal + context, no new information
    context_snapshot,   // altstrategy.decision_context_snapshots scalar columns
    context_analytics,  // the named analytics persisted in that row's snapshot
    prior,              // as-of historical behaviour (matured outcomes only)
};

// The only legal availability. It exists as a field, rather than as a comment,
// so `leakage.hpp` can assert it mechanically over the whole contract.
enum class Availability : uint8_t { at_signal_time };

struct FeatureSpec {
    std::string_view name;
    FeatureKind kind;
    FeatureSource source;
    Availability availability;
};

[[nodiscard]] constexpr std::string_view kind_name(FeatureKind k) noexcept {
    switch (k) {
        case FeatureKind::numeric: return "numeric";
        case FeatureKind::binary: return "binary";
        case FeatureKind::ordinal: return "ordinal";
        case FeatureKind::cyclic: return "cyclic";
    }
    return "?";
}

[[nodiscard]] constexpr std::string_view source_name(FeatureSource s) noexcept {
    switch (s) {
        case FeatureSource::proposal: return "proposal";
        case FeatureSource::derived: return "derived";
        case FeatureSource::context_snapshot: return "context_snapshot";
        case FeatureSource::context_analytics: return "context_analytics";
        case FeatureSource::prior: return "prior";
    }
    return "?";
}

[[nodiscard]] constexpr std::string_view availability_name(Availability a) noexcept {
    switch (a) {
        case Availability::at_signal_time: return "AT_SIGNAL_TIME";
    }
    return "?";
}

namespace detail {
constexpr FeatureSpec f(std::string_view name, FeatureKind kind, FeatureSource source) {
    return FeatureSpec{name, kind, source, Availability::at_signal_time};
}
using K = FeatureKind;
using S = FeatureSource;
} // namespace detail

// ---------------------------------------------------------------------------
// sq-core:6 — 109 members. Order is frozen.
// ---------------------------------------------------------------------------
inline constexpr std::array<FeatureSpec, 109> kFeatures = {{
    // --- the candidate itself (0..38) --------------------------------------
    detail::f("side_is_long", detail::K::binary, detail::S::proposal),
    detail::f("grade_ord", detail::K::ordinal, detail::S::proposal),
    detail::f("upstream_recommended", detail::K::binary, detail::S::proposal),
    detail::f("entry_type_ord", detail::K::ordinal, detail::S::proposal),
    detail::f("holding_class_ord", detail::K::ordinal, detail::S::proposal),
    detail::f("plan_version", detail::K::ordinal, detail::S::proposal),
    detail::f("context_quality_ord", detail::K::ordinal, detail::S::proposal),
    detail::f("expected_rr", detail::K::numeric, detail::S::proposal),
    detail::f("stop_distance_bps", detail::K::numeric, detail::S::proposal),
    detail::f("stop_distance_atr", detail::K::numeric, detail::S::proposal),
    detail::f("target_distance_bps", detail::K::numeric, detail::S::proposal),
    detail::f("max_slippage_bps", detail::K::numeric, detail::S::proposal),
    detail::f("post_only_allowed", detail::K::binary, detail::S::proposal),
    detail::f("expected_fill_time_p50_min", detail::K::numeric, detail::S::proposal),
    detail::f("expected_fill_time_p90_min", detail::K::numeric, detail::S::proposal),
    detail::f("entry_fill_timeout_min", detail::K::numeric, detail::S::proposal),
    detail::f("expected_holding_p50_min", detail::K::numeric, detail::S::proposal),
    detail::f("expected_holding_p90_min", detail::K::numeric, detail::S::proposal),
    detail::f("max_holding_min", detail::K::numeric, detail::S::proposal),
    detail::f("signal_validity_min", detail::K::numeric, detail::S::proposal),
    // Forward-looking economics. AltStrategy writes these ONLY when a model
    // produced them, and says so with `edge_estimated`; when it is false they
    // arrive absent and must be recorded absent, not as zero.
    //
    // Eight members were removed here in sq-core:3 -- p_entry_reached,
    // p_stop_before_target, p_timeout, p_ambiguous, expected_gross_return,
    // expected_utility, ml_uncertainty and ml_ood. AltStrategy stopped
    // producing all eight; protobuf returned 0 for each, and the extractor
    // marked them PRESENT, so the trainer saw eight constant zeros it believed
    // were measurements. The runtime diagnostic said so plainly -- "constant":
    // 8 out of 110 declared -- for every training cycle.
    detail::f("edge_estimated", detail::K::binary, detail::S::proposal),
    detail::f("p_target_before_stop", detail::K::numeric, detail::S::proposal),
    detail::f("expected_net_return", detail::K::numeric, detail::S::proposal),
    detail::f("ml_conviction", detail::K::numeric, detail::S::proposal),
    detail::f("ml_abstain", detail::K::binary, detail::S::proposal),
    detail::f("ensemble_agreement", detail::K::numeric, detail::S::proposal),
    detail::f("adaptive_score", detail::K::numeric, detail::S::proposal),
    detail::f("risk_flag_count", detail::K::numeric, detail::S::proposal),
    detail::f("warning_count", detail::K::numeric, detail::S::proposal),
    detail::f("reason_code_count", detail::K::numeric, detail::S::proposal),
    detail::f("alternative_plan_count", detail::K::numeric, detail::S::proposal),
    detail::f("hour_of_day_sin", detail::K::cyclic, detail::S::proposal),
    detail::f("hour_of_day_cos", detail::K::cyclic, detail::S::proposal),

    // --- arithmetic over what is already above (39..43) ---------------------
    detail::f("entry_offset_bps", detail::K::numeric, detail::S::derived),
    detail::f("stop_to_target_ratio", detail::K::numeric, detail::S::derived),
    detail::f("cost_to_target_ratio", detail::K::numeric, detail::S::derived),
    detail::f("net_edge_bps", detail::K::numeric, detail::S::derived),
    detail::f("rr_times_p_target", detail::K::numeric, detail::S::derived),

    // --- the point-in-time context row (44..57) -----------------------------
    detail::f("spread_bps", detail::K::numeric, detail::S::context_snapshot),
    detail::f("log_top_depth_usdt", detail::K::numeric, detail::S::context_snapshot),
    detail::f("liquidity_rank", detail::K::numeric, detail::S::context_snapshot),
    detail::f("expected_slippage_bps", detail::K::numeric, detail::S::context_snapshot),
    detail::f("fees_bps", detail::K::numeric, detail::S::context_snapshot),
    detail::f("funding_rate", detail::K::numeric, detail::S::context_snapshot),
    detail::f("data_quality_ord", detail::K::ordinal, detail::S::context_snapshot),
    detail::f("venue_count", detail::K::numeric, detail::S::context_snapshot),
    detail::f("missing_venue_count", detail::K::numeric, detail::S::context_snapshot),
    detail::f("regime_divergence", detail::K::binary, detail::S::context_snapshot),
    detail::f("bitget_regime_ord", detail::K::ordinal, detail::S::context_snapshot),
    detail::f("global_regime_ord", detail::K::ordinal, detail::S::context_snapshot),
    detail::f("context_age_at_signal_ms", detail::K::numeric, detail::S::context_snapshot),
    detail::f("context_available_lag_ms", detail::K::numeric, detail::S::context_snapshot),

    // --- the analytics AltStrategy decided on (58..91) -----------------------
    detail::f("atr_14_bps", detail::K::numeric, detail::S::context_analytics),
    detail::f("atr_expansion", detail::K::numeric, detail::S::context_analytics),
    detail::f("adx_14", detail::K::numeric, detail::S::context_analytics),
    detail::f("rsi_14", detail::K::numeric, detail::S::context_analytics),
    detail::f("roc_14", detail::K::numeric, detail::S::context_analytics),
    detail::f("mom_14", detail::K::numeric, detail::S::context_analytics),
    detail::f("bb_width_bps", detail::K::numeric, detail::S::context_analytics),
    detail::f("ema_20_50_spread_bps", detail::K::numeric, detail::S::context_analytics),
    detail::f("price_vs_ema200_bps", detail::K::numeric, detail::S::context_analytics),
    detail::f("donchian_position_20", detail::K::numeric, detail::S::context_analytics),
    detail::f("stddev_20_bps", detail::K::numeric, detail::S::context_analytics),
    detail::f("micro_trade_imbalance", detail::K::numeric, detail::S::context_analytics),
    detail::f("micro_book_imbalance", detail::K::numeric, detail::S::context_analytics),
    detail::f("micro_cvd", detail::K::numeric, detail::S::context_analytics),
    detail::f("micro_cvd_delta", detail::K::numeric, detail::S::context_analytics),
    detail::f("micro_trade_intensity", detail::K::numeric, detail::S::context_analytics),
    detail::f("micro_signed_volume", detail::K::numeric, detail::S::context_analytics),
    detail::f("micro_turnover_acceleration", detail::K::numeric, detail::S::context_analytics),
    detail::f("micro_microprice_deviation", detail::K::numeric, detail::S::context_analytics),
    detail::f("micro_bid_depth_share", detail::K::numeric, detail::S::context_analytics),
    detail::f("xv_dislocation_bps", detail::K::numeric, detail::S::context_analytics),
    detail::f("xv_price_dispersion_bps", detail::K::numeric, detail::S::context_analytics),
    detail::f("xv_momentum_agreement", detail::K::numeric, detail::S::context_analytics),
    detail::f("xv_orderflow_agreement", detail::K::numeric, detail::S::context_analytics),
    detail::f("xv_funding_divergence", detail::K::numeric, detail::S::context_analytics),
    detail::f("xv_oi_growth", detail::K::numeric, detail::S::context_analytics),
    detail::f("xv_confidence", detail::K::numeric, detail::S::context_analytics),
    detail::f("xv_rsi_dispersion", detail::K::numeric, detail::S::context_analytics),
    detail::f("xv_volatility_agreement", detail::K::numeric, detail::S::context_analytics),
    detail::f("xv_trend_alignment_agreement", detail::K::numeric, detail::S::context_analytics),
    detail::f("regime_confidence", detail::K::numeric, detail::S::context_analytics),
    detail::f("regime_trend_strength", detail::K::numeric, detail::S::context_analytics),
    detail::f("regime_volatility", detail::K::numeric, detail::S::context_analytics),
    detail::f("regime_liquidity_quality", detail::K::numeric, detail::S::context_analytics),

    // --- higher-timeframe context (§43) --------------------------------------
    // AltStrategy admits a higher grid only once its own bar has CLOSED, so
    // these are point-in-time by construction. The shortest available grid
    // above the execution one is used. All are ABSENT when no higher grid had
    // closed at T0 -- which is the honest state after a restart, and common.
    //
    // Whether the higher frame carries information about signal quality is a
    // question, not an assumption: `ablation --arm no_htf` measures it.
    detail::f("htf_minutes", detail::K::numeric, detail::S::context_analytics),
    detail::f("htf_adx_14", detail::K::numeric, detail::S::context_analytics),
    detail::f("htf_rsi_14", detail::K::numeric, detail::S::context_analytics),
    detail::f("htf_ema_20_50_spread_bps", detail::K::numeric, detail::S::context_analytics),
    detail::f("htf_trend_agrees_with_side", detail::K::numeric, detail::S::derived),

    // --- as-of historical behaviour (92..109) -------------------------------
    detail::f("prior_global_win_rate", detail::K::numeric, detail::S::prior),
    detail::f("prior_global_expectancy", detail::K::numeric, detail::S::prior),
    detail::f("prior_strategy_win_rate", detail::K::numeric, detail::S::prior),
    detail::f("prior_strategy_expectancy", detail::K::numeric, detail::S::prior),
    detail::f("prior_strategy_ess", detail::K::numeric, detail::S::prior),
    detail::f("prior_pair_win_rate", detail::K::numeric, detail::S::prior),
    detail::f("prior_pair_expectancy", detail::K::numeric, detail::S::prior),
    detail::f("prior_pair_ess", detail::K::numeric, detail::S::prior),
    detail::f("prior_regime_win_rate", detail::K::numeric, detail::S::prior),
    detail::f("prior_regime_expectancy", detail::K::numeric, detail::S::prior),
    detail::f("prior_strategy_regime_win_rate", detail::K::numeric, detail::S::prior),
    detail::f("prior_strategy_regime_expectancy", detail::K::numeric, detail::S::prior),
    detail::f("prior_strategy_regime_ess", detail::K::numeric, detail::S::prior),
    detail::f("prior_strategy_side_win_rate", detail::K::numeric, detail::S::prior),
    detail::f("prior_strategy_side_expectancy", detail::K::numeric, detail::S::prior),
    detail::f("prior_grade_win_rate", detail::K::numeric, detail::S::prior),
    detail::f("prior_decayed_strategy_win_rate", detail::K::numeric, detail::S::prior),
    detail::f("prior_decayed_strategy_expectancy", detail::K::numeric, detail::S::prior),
}};

inline constexpr size_t kFeatureCount = kFeatures.size();

// §57 ablation: which declared members belong to a named input group.
//
// The groups are the questions worth asking of this contract -- does the ML
// evidence carry quality information, does cross-venue, does microstructure,
// does the higher timeframe -- and they are matched by the naming convention
// the contract already follows, so a new member joins its group automatically.
[[nodiscard]] inline bool feature_in_group(std::string_view name, std::string_view group) {
    const auto starts = [&](std::string_view p) { return name.substr(0, p.size()) == p; };
    if (group == "ml") {
        return name == "edge_estimated" || name == "p_target_before_stop" ||
               name == "expected_net_return" || name == "ml_conviction" || name == "ml_abstain";
    }
    if (group == "cross_venue") return starts("xv_");
    if (group == "microstructure") return starts("micro_");
    if (group == "htf") return starts("htf_");
    if (group == "regime") return starts("regime_");
    if (group == "prior") return starts("prior_");
    return false;
}

// Blank every member of the named groups across the whole matrix, marking them
// ABSENT (NaN) rather than zero. Returns how many members were blanked.
[[nodiscard]] inline size_t ablate(std::vector<float>& X, size_t rows,
                                   const std::vector<std::string>& groups) {
    std::vector<size_t> positions;
    for (size_t i = 0; i < kFeatures.size(); ++i) {
        for (const auto& g : groups) {
            if (feature_in_group(kFeatures[i].name, g)) { positions.push_back(i); break; }
        }
    }
    for (size_t r = 0; r < rows; ++r) {
        for (size_t pos : positions) X[r * kFeatureCount + pos] = std::numeric_limits<float>::quiet_NaN();
    }
    return positions.size();
}

// ---------------------------------------------------------------------------
// Assembly
// ---------------------------------------------------------------------------

struct FeatureVector {
    std::vector<float> values;      // NaN where absent
    std::vector<uint8_t> validity;  // little-endian bit i => values[i] observed
    int32_t missing_count = 0;

    [[nodiscard]] bool observed(size_t i) const {
        return (i / 8) < validity.size() && (((validity[i / 8] >> (i % 8)) & 1U) != 0U);
    }
};

namespace detail {

class Writer {
public:
    explicit Writer(FeatureVector& out) : out_(out) {
        out_.values.reserve(kFeatureCount);
        out_.validity.assign((kFeatureCount + 7) / 8, 0);
        out_.missing_count = 0;
    }

    void put(std::optional<double> v) {
        const size_t i = out_.values.size();
        if (v.has_value() && std::isfinite(*v)) {
            out_.values.push_back(static_cast<float>(*v));
            out_.validity[i / 8] = static_cast<uint8_t>(out_.validity[i / 8] | (1U << (i % 8)));
        } else {
            out_.values.push_back(std::numeric_limits<float>::quiet_NaN());
            ++out_.missing_count;
        }
    }

    // An ordinal whose token was outside the frozen vocabulary is missing, not
    // the sentinel: -1 next to 1..9 would be a category the model can split on.
    void put_ordinal(double ordinal) {
        if (ordinal == kUnknownOrdinal) {
            put(std::nullopt);
        } else {
            put(ordinal);
        }
    }

    void put_bool(bool v) { put(v ? 1.0 : 0.0); }

private:
    FeatureVector& out_;
};

// Safe ratio: a zero or non-finite denominator yields "absent", never inf.
inline std::optional<double> ratio(std::optional<double> num, std::optional<double> den) {
    if (!num || !den || !std::isfinite(*num) || !std::isfinite(*den) || *den == 0.0) {
        return std::nullopt;
    }
    return *num / *den;
}

inline std::optional<double> some(double v) {
    if (!std::isfinite(v)) return std::nullopt;
    return v;
}

// Relative distance in basis points, guarding a zero reference price.
inline std::optional<double> bps_of(std::optional<double> value, std::optional<double> reference) {
    auto r = ratio(value, reference);
    if (!r) return std::nullopt;
    return *r * 10'000.0;
}

} // namespace detail

// Build the positional feature vector for one signal. Pure: same snapshot in,
// same vector out, on any host, in any order, forever.
[[nodiscard]] FeatureVector assemble(const SignalSnapshot& s);

// sha256 over "position|name|kind|source|availability" for every member. Two
// contracts with the same hash are the same contract, member for member.
[[nodiscard]] std::string feature_schema_hash();

// The contract as an ordered JSON array, for altsignalquality.feature_schemas
// and the model's feature_names.json artifact.
[[nodiscard]] std::string feature_contract_json();

// LightGBM feature names, in contract order, prefixed so a raw booster dump is
// unambiguous about which contract produced it.
[[nodiscard]] std::vector<std::string> lgbm_feature_names();

} // namespace altsignalquality::domain
