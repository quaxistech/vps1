#include "altsignalquality/domain/feature_contract.hpp"

#include <algorithm>

#include "altsignalquality/core/hash.hpp"
#include "altsignalquality/core/time.hpp"

#include <nlohmann/json.hpp>

#include <numbers>
#include <stdexcept>

namespace altsignalquality::domain {

namespace {

using detail::bps_of;
using detail::ratio;
using detail::some;

std::optional<double> ctx_value(const ContextSnapshot& ctx,
                                const std::map<std::string, double>& m,
                                const char* key) {
    if (!ctx.present) return std::nullopt;
    const auto it = m.find(key);
    if (it == m.end() || !std::isfinite(it->second)) return std::nullopt;
    return it->second;
}

void write_priors(detail::Writer& w, const PriorSnapshot& p) {
    const auto cell_rate = [](const PriorCell& c) -> std::optional<double> {
        if (!c.present) return std::nullopt;
        return c.win_rate;
    };
    const auto cell_exp = [](const PriorCell& c) -> std::optional<double> {
        if (!c.present) return std::nullopt;
        return c.expectancy;
    };
    const auto cell_ess = [](const PriorCell& c) -> std::optional<double> {
        if (!c.present) return std::nullopt;
        return c.effective_samples;
    };

    w.put(cell_rate(p.global));
    w.put(cell_exp(p.global));
    w.put(cell_rate(p.strategy));
    w.put(cell_exp(p.strategy));
    w.put(cell_ess(p.strategy));
    w.put(cell_rate(p.pair));
    w.put(cell_exp(p.pair));
    w.put(cell_ess(p.pair));
    w.put(cell_rate(p.regime));
    w.put(cell_exp(p.regime));
    w.put(cell_rate(p.strategy_regime));
    w.put(cell_exp(p.strategy_regime));
    w.put(cell_ess(p.strategy_regime));
    w.put(cell_rate(p.strategy_side));
    w.put(cell_exp(p.strategy_side));
    w.put(cell_rate(p.grade));
    w.put(cell_rate(p.decayed_strategy));
    w.put(cell_exp(p.decayed_strategy));
}

} // namespace

FeatureVector assemble(const SignalSnapshot& s) {
    const CandidateSignal& c = s.candidate;
    const ContextSnapshot& ctx = s.context;

    // The reference price for every relative-distance feature. Present only
    // when the context row itself is, so a missing context cannot leak a stale
    // mark into an offset — and only when the price is POSITIVE.
    //
    // A zero or negative mark is not a price; it is a broken row. Arithmetic on
    // it still produces finite numbers — `(mark - ema200) / ema200` happily
    // returns −10 000 bps for mark = 0 — so nothing downstream would notice,
    // and the model would learn from a value that never existed in the market.
    const std::optional<double> mark =
        (ctx.present && ctx.mark_price && *ctx.mark_price > 0.0) ? ctx.mark_price
                                                                 : std::nullopt;

    FeatureVector out;
    detail::Writer w(out);

    // --- the candidate itself -------------------------------------------------
    w.put_bool(c.side == 1);                       // SIDE_LONG
    w.put_ordinal(grade_ordinal(c.proposal_grade));
    w.put_bool(c.recommended_for_execution);
    w.put_ordinal(entry_type_ordinal(c.entry_type));
    w.put_ordinal(holding_class_ordinal(c.holding_class));
    w.put(static_cast<double>(c.plan_version));
    w.put_ordinal(context_quality_ordinal(c.quality));
    w.put(some(c.expected_rr));
    w.put(some(c.stop_distance_bps));
    w.put(some(c.stop_distance_atr));
    w.put(some(c.target_distance_bps));
    w.put(some(c.max_slippage_bps));
    w.put_bool(c.post_only_allowed);
    w.put(some(c.expected_fill_time_p50_min));
    w.put(some(c.expected_fill_time_p90_min));
    w.put(some(static_cast<double>(c.entry_fill_timeout_ms) / 60'000.0));
    w.put(some(static_cast<double>(c.expected_holding_time_p50_ms) / 60'000.0));
    w.put(some(static_cast<double>(c.expected_holding_time_p90_ms) / 60'000.0));
    w.put(some(static_cast<double>(c.max_holding_time_ms) / 60'000.0));
    // A non-positive validity window means the proposal contract was violated
    // upstream; report absent rather than a negative "duration".
    w.put(c.valid_until_ms > c.created_at_ms
              ? some(static_cast<double>(c.valid_until_ms - c.created_at_ms) / 60'000.0)
              : std::nullopt);
    // Economics are present only when AltStrategy had a model to produce them.
    // Writing some(0.0) here would tell the trainer "the expected net return is
    // zero", which is a measurement; absent says "nobody estimated it", which is
    // the truth. AltStrategy carries the distinction in `edge_estimated` and
    // writes NULL for the figures themselves.
    w.put_bool(c.edge_estimated);
    w.put(c.edge_estimated && c.p_target_before_stop ? some(*c.p_target_before_stop) : std::nullopt);
    w.put(c.edge_estimated ? some(c.expected_net_return) : std::nullopt);
    // Conviction is |p_up - 0.5| * 2 -- distance from a coin flip. It is
    // meaningless when the model abstained, so it is absent then.
    w.put(c.ml_abstain ? std::nullopt : some(c.ml_conviction));
    w.put_bool(c.ml_abstain);
    w.put(some(c.ensemble_agreement));
    w.put(some(c.adaptive_score));
    w.put(static_cast<double>(c.risk_flag_count));
    w.put(static_cast<double>(c.warning_count));
    w.put(static_cast<double>(c.reason_code_count));
    w.put(static_cast<double>(c.alternative_plan_count));
    // Time of day as a circle, so 23:59 and 00:01 are adjacent rather than
    // maximally far apart. Derived from T0 only.
    {
        const double hour = timeutil::utc_hour_of_day(c.created_at_ms);
        const double angle = 2.0 * std::numbers::pi * hour / 24.0;
        w.put(c.created_at_ms > 0 ? some(std::sin(angle)) : std::nullopt);
        w.put(c.created_at_ms > 0 ? some(std::cos(angle)) : std::nullopt);
    }

    // --- arithmetic over what is already known --------------------------------
    // How far the entry sits from the reference mark, signed TOWARD the trade:
    // positive means the entry is better than mark for this side.
    w.put([&]() -> std::optional<double> {
        // Both sides of the comparison must be real prices.
        if (!mark || !(c.entry_price > 0.0)) return std::nullopt;
        auto offset = bps_of(some(c.entry_price - *mark), mark);
        if (!offset) return std::nullopt;
        return -*offset * side_sign(c.side);
    }());
    w.put(ratio(some(c.stop_distance_bps), some(c.target_distance_bps)));
    w.put([&]() -> std::optional<double> {
        // Every component is required. Substituting zero for an absent one
        // reports a cost that is too LOW — the direction that makes a trade
        // look better than it is — and the model cannot tell that number apart
        // from a genuinely cheap venue. An incomplete cost picture is MISSING.
        if (!ctx.present || !ctx.expected_slippage_bps || !ctx.spread_bps) {
            return std::nullopt;
        }
        // AltStrategy's cost model (engine/cost_model.hpp v2), reproduced
        // exactly. This used to be `2 * fees_bps + expected_slippage_bps +
        // spread_bps`, which is the formula that model documents as wrong on
        // two counts:
        //
        //   * `expected_slippage_bps` already contains half the spread, so
        //     adding the full spread on top charges 1.5x the spread for a round
        //     trip that crosses it at half each time;
        //   * `ctx.fees_bps` is a single blended configuration default (4.0 on
        //     this deployment), so the cost did not move with the entry type at
        //     all -- while the real round trip runs 11.9 bps for a resting
        //     limit entry and 17.3 for a market one. Measured against the
        //     producer's own `round_trip_cost_bps` over six hours of live
        //     context: this feature said 14.7 bps for every signal, the desk
        //     paid 15.1 on average and never that number on any single trade.
        //
        // Fees are per LEG, from altml.fee_models v1 (Bitget USDT-M perpetuals,
        // maker 0.0002, taker 0.0006). The exit always crosses the book: a
        // protective stop is a market order, and a take-profit limit that does
        // not fill is not an exit.
        constexpr double kMakerFeeBps = 2.0;
        constexpr double kTakerFeeBps = 6.0;
        // ENTRY_MARKET (1) and ENTRY_STOP_MARKET (3) cross the book; the limit
        // forms rest and earn the spread instead of paying it.
        const bool taker_entry = c.entry_type == 1 || c.entry_type == 3;
        const double half_spread = std::max(0.0, *ctx.spread_bps) / 2.0;
        const double fees = (taker_entry ? kTakerFeeBps : kMakerFeeBps) + kTakerFeeBps;
        const double spread_cost = half_spread * (taker_entry ? 2.0 : 1.0);
        const double slippage_cost = std::max(0.0, *ctx.expected_slippage_bps - half_spread);
        return ratio(some(fees + spread_cost + slippage_cost), some(c.target_distance_bps));
    }());
    // Both are arithmetic over the forward-looking economics above, so they
    // inherit that block's condition. Writing them unconditionally reported a
    // net edge of exactly 0.0 bps and an RR-weighted target probability of
    // exactly 0.0 on every signal AltStrategy did not estimate -- measured
    // constant across all 3752 archived examples, because `edge_estimated` is
    // false whenever AltML has no active model. That is a measurement the
    // service never made, stated in the units of one it did.
    w.put(c.edge_estimated ? some(c.expected_net_return * 10'000.0) : std::nullopt);
    w.put(c.edge_estimated && c.p_target_before_stop
              ? some(c.expected_rr * *c.p_target_before_stop)
              : std::nullopt);

    // --- the point-in-time context row ---------------------------------------
    w.put(ctx.present ? ctx.spread_bps : std::nullopt);
    w.put([&]() -> std::optional<double> {
        if (!ctx.present || !ctx.top_depth_usdt || *ctx.top_depth_usdt < 0.0) return std::nullopt;
        return std::log1p(*ctx.top_depth_usdt);
    }());
    w.put(ctx.present ? ctx.liquidity_rank : std::nullopt);
    w.put(ctx.present ? ctx.expected_slippage_bps : std::nullopt);
    w.put(ctx.present ? ctx.fees_bps : std::nullopt);
    w.put(ctx.present ? ctx.funding_rate : std::nullopt);
    w.put_ordinal(ctx.present ? context_quality_ordinal(ctx.data_quality) : kUnknownOrdinal);
    w.put(ctx.present ? some(venue_count(ctx.venue_mask)) : std::nullopt);
    w.put(ctx.present ? some(venue_count(ctx.missing_venue_mask)) : std::nullopt);
    w.put(ctx.present ? std::optional<double>(ctx.regime_divergence ? 1.0 : 0.0) : std::nullopt);
    w.put_ordinal(ctx.present ? regime_ordinal(ctx.bitget_regime) : kUnknownOrdinal);
    w.put_ordinal(ctx.present ? regime_ordinal(ctx.global_regime) : kUnknownOrdinal);
    // How stale the market picture already was when the signal was formed, and
    // how long the inputs took to become usable. Both are pure T0 arithmetic.
    w.put(ctx.present && ctx.event_time_ms > 0
              ? some(static_cast<double>(c.created_at_ms - ctx.event_time_ms))
              : std::nullopt);
    w.put(ctx.present && ctx.event_time_ms > 0 && ctx.available_at_ms > 0
              ? some(static_cast<double>(ctx.available_at_ms - ctx.event_time_ms))
              : std::nullopt);

    // --- the analytics AltStrategy decided on --------------------------------
    const auto bf = [&](const char* k) { return ctx_value(ctx, ctx.bitget_features, k); };
    const auto mf = [&](const char* k) { return ctx_value(ctx, ctx.microstructure, k); };
    const auto xf = [&](const char* k) { return ctx_value(ctx, ctx.cross_venue, k); };

    w.put(bps_of(bf("atr_14"), mark));
    w.put(ratio(bf("atr_14"), bf("atr_50")));
    w.put(bf("adx_14"));
    w.put(bf("rsi_14"));
    w.put(bf("roc_14"));
    w.put(bf("mom_14"));
    w.put([&]() -> std::optional<double> {
        auto up = bf("bb_upper_20"), lo = bf("bb_lower_20"), mid = bf("bb_middle_20");
        if (!up || !lo) return std::nullopt;
        return bps_of(some(*up - *lo), mid);
    }());
    w.put([&]() -> std::optional<double> {
        auto e20 = bf("ema_20"), e50 = bf("ema_50");
        if (!e20 || !e50) return std::nullopt;
        return bps_of(some(*e20 - *e50), e50);
    }());
    w.put([&]() -> std::optional<double> {
        auto e200 = bf("ema_200");
        if (!e200 || !mark) return std::nullopt;
        return bps_of(some(*mark - *e200), e200);
    }());
    w.put([&]() -> std::optional<double> {
        auto hi = bf("donchian_high_20"), lo = bf("donchian_low_20");
        if (!hi || !lo || !mark || *hi <= *lo) return std::nullopt;
        return (*mark - *lo) / (*hi - *lo);
    }());
    w.put(bps_of(bf("stddev_20"), mark));
    w.put(mf("trade_imbalance"));
    w.put(mf("book_imbalance"));
    w.put(mf("cvd"));
    w.put(mf("cvd_delta"));
    w.put(mf("trade_intensity"));
    w.put(mf("signed_volume"));
    w.put(mf("turnover_acceleration"));
    w.put(mf("book_microprice_dev"));
    w.put([&]() -> std::optional<double> {
        auto bid = mf("book_bid_depth"), ask = mf("book_ask_depth");
        if (!bid || !ask || (*bid + *ask) <= 0.0) return std::nullopt;
        return *bid / (*bid + *ask);
    }());
    w.put(xf("dislocation_bps"));
    w.put(xf("venue_price_dispersion_bps"));
    w.put(xf("momentum_agreement"));
    w.put(xf("breakout_agreement"));
    w.put(xf("funding_divergence"));
    w.put(xf("oi_growth"));
    w.put(xf("cross_venue_confidence"));
    w.put(xf("rsi_dispersion"));
    w.put(xf("volatility_agreement"));
    w.put(xf("trend_alignment_agreement"));
    w.put(ctx.present ? ctx.regime_confidence : std::nullopt);
    w.put(ctx.present ? ctx.regime_trend_strength : std::nullopt);
    w.put(ctx.present ? ctx.regime_volatility : std::nullopt);
    w.put(ctx.present ? ctx.regime_liquidity_quality : std::nullopt);

    // --- higher-timeframe context (§43) --------------------------------------
    const auto hf = [&](const char* k) -> std::optional<double> {
        if (!ctx.present || ctx.htf_minutes <= 0) return std::nullopt;
        const auto it = ctx.htf_features.find(k);
        return it == ctx.htf_features.end() ? std::nullopt : some(it->second);
    };
    w.put(ctx.present && ctx.htf_minutes > 0 ? some(static_cast<double>(ctx.htf_minutes))
                                             : std::nullopt);
    w.put(hf("adx_14"));
    w.put(hf("rsi_14"));
    const auto htf_spread = [&]() -> std::optional<double> {
        auto e20 = hf("ema_20"), e50 = hf("ema_50");
        if (!e20 || !e50) return std::nullopt;
        return bps_of(some(*e20 - *e50), mark);
    }();
    w.put(htf_spread);
    // +1 when the higher frame's trend points the way this signal does, -1 when
    // it points against, absent when the frame is missing. This is the question
    // the higher grid exists to answer for a quality model.
    w.put([&]() -> std::optional<double> {
        if (!htf_spread) return std::nullopt;
        const bool htf_up = *htf_spread > 0.0;
        const bool signal_long = c.side == 1;
        return some(htf_up == signal_long ? 1.0 : -1.0);
    }());

    // --- as-of historical behaviour ------------------------------------------
    write_priors(w, s.priors);

    // A vector of the wrong length would silently shift every position after
    // the mistake, which is exactly the failure the positional contract exists
    // to prevent. Fail loudly instead.
    if (out.values.size() != kFeatureCount) {
        throw std::logic_error("feature assembly produced " +
                               std::to_string(out.values.size()) + " values, contract declares " +
                               std::to_string(kFeatureCount));
    }
    return out;
}

std::string feature_schema_hash() {
    static const std::string cached = [] {
        std::string canonical;
        canonical.reserve(kFeatureCount * 64);
        canonical += kFeatureSchemaVersion;
        canonical += '\n';
        for (size_t i = 0; i < kFeatures.size(); ++i) {
            const auto& f = kFeatures[i];
            canonical += std::to_string(i);
            canonical += '|';
            canonical += f.name;
            canonical += '|';
            canonical += kind_name(f.kind);
            canonical += '|';
            canonical += source_name(f.source);
            canonical += '|';
            canonical += availability_name(f.availability);
            canonical += '\n';
        }
        return sha256_hex(canonical);
    }();
    return cached;
}

std::string feature_contract_json() {
    nlohmann::json arr = nlohmann::json::array();
    for (size_t i = 0; i < kFeatures.size(); ++i) {
        const auto& f = kFeatures[i];
        arr.push_back({{"position", i},
                       {"name", std::string(f.name)},
                       {"kind", std::string(kind_name(f.kind))},
                       {"source", std::string(source_name(f.source))},
                       {"availability", std::string(availability_name(f.availability))}});
    }
    return arr.dump();
}

std::vector<std::string> lgbm_feature_names() {
    std::vector<std::string> names;
    names.reserve(kFeatureCount);
    for (const auto& f : kFeatures) names.push_back("sq_" + std::string(f.name));
    return names;
}

int32_t undeclared_missing_count(const FeatureVector& v, const CandidateSignal& c) {
    // Exactly the gates assemble() applies to these four members.
    const auto declared_absent = [&](std::string_view name) {
        const bool target_probability = name == "p_target_before_stop" || name == "rr_times_p_target";
        const bool edge = name == "expected_net_return" || name == "net_edge_bps";
        if (!c.edge_estimated) return target_probability || edge;
        return target_probability && !c.p_target_before_stop.has_value();
    };
    int32_t n = 0;
    for (size_t i = 0; i < kFeatureCount; ++i)
        if (!v.observed(i) && !declared_absent(kFeatures[i].name)) ++n;
    return n;
}

} // namespace altsignalquality::domain
