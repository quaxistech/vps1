#include "altstrategy/engine/trade_plan.hpp"

#include "altstrategy/engine/ids.hpp"
#include "altstrategy/engine/signal_predictions.hpp"
#include "altstrategy/engine/state_candidates.hpp"
#include "altstrategy/engine/units.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace altstrategy::engine {

namespace {

// A tick guessed from the price magnitude (roughly one part in 10^4..10^5,
// clamped). Only a fallback: against the venue's own grid it is wrong by 10x
// for most of the universe in one direction or the other (ADAUSDT trades in
// 0.0001, this says 0.00001; BTCUSDT in 0.1, this says 1).
double derive_tick(double price) {
    if (price <= 0) return 0.0001;
    const double mag = std::pow(10.0, std::floor(std::log10(price)) - 4.0);
    return std::clamp(mag, 1e-8, 1.0);
}

// Distances from the (on-grid) entry in whole ticks, so a LONG and its mirror
// SHORT round identically -- rounding the absolute PRICE broke ties away from
// zero, which put a long's target a tick further out than its mirror short's.
// A stop rounds UP: it is sized from floors (the invalidation, 0.8 ATR,
// min_stop_bps, the spread), and the nearest tick can sit inside them by half
// a tick, which on a coarse grid is not small (DOTUSDT: one tick is ~9 bps).
// The epsilon keeps a distance already on the grid where it is. Never fewer
// than one tick: a level on the entry is no level.
int64_t stop_ticks(double distance, double tick) {
    constexpr double kOnGrid = 1e-9;
    return std::max<int64_t>(1, static_cast<int64_t>(std::ceil(distance / tick - kOnGrid)));
}
int64_t target_ticks(double distance, double tick) {
    return std::max<int64_t>(1, std::llround(distance / tick));
}

int64_t holding_cap(HoldingClass hc, const PlanConfig& cfg) {
    switch (hc) {
        case HoldingClass::scalp: return cfg.scalp_max_ms;
        case HoldingClass::extended_intraday: return cfg.extended_max_ms;
        default: return cfg.intraday_max_ms;
    }
}

// The expected gross return of the candidate's side, in bps, from the
// prediction that may shape its plan; 0 without one. AltML's expected return
// is the LONG perspective, so a short is favoured by a negative one.
double side_expected_return_bps(const StrategyCandidate& c) {
    const auto* h = geometry_prediction(c);
    if (h == nullptr) return 0.0;
    return (c.side == Side::long_ ? 1.0 : c.side == Side::short_ ? -1.0 : 0.0) * h->expected_return_bps;
}

} // namespace

// Exposed in the header: the ranking prices the same entry leg the plan will
// use, so the two cannot disagree about whether a fill is maker or taker.
EntryType choose_entry(const StrategyCandidate& c, const DecisionContext& ctx) {
    // STOP entry for breakouts / volatility expansion; MARKET for a strong
    // immediate impulse with depth + normal spread + large edge; LIMIT otherwise
    // (pullback / vwap reclaim / structure retest).
    const auto has = [&](const char* code) {
        for (const auto& r : c.reason_codes) if (r == code) return true;
        return false;
    };
    // A full-state thesis is the model's estimate of a TAKER entry held for the
    // horizon (econ target, cost model v2): anything else would execute a
    // different trade than the one whose economics were estimated.
    if (c.strategy_id == kStateStrategyId) return EntryType::market;
    if (c.strategy_family == Family::breakout || has("donchian_breakout") || has("expansion_confirmed"))
        return EntryType::stop_market;
    // A large expected move FOR THIS SIDE. The test was |expected_return| >
    // 0.003, so a model expecting a 30 bps fall made a LONG "strong" enough to
    // cross the spread -- and it read any forecast present, SHADOW or not.
    constexpr double kStrongEdgeBps = 30.0;
    const bool strong = c.raw_setup_score > 0.7 && ctx.spread_bps < 5.0 && ctx.top_depth_usdt > 50000 &&
                        side_expected_return_bps(c) > kStrongEdgeBps;
    if (strong && (c.strategy_family == Family::trend || c.strategy_family == Family::cross_venue))
        return EntryType::market;
    return EntryType::limit;
}

const PriceTickVersion* price_tick_at(const PriceTickTable& table, int64_t pair, int64_t t_ms) {
    const auto it = table.find(pair);
    if (it == table.end()) return nullptr;
    const PriceTickVersion* best = nullptr;
    for (const auto& v : it->second) {
        if (v.valid_from_ms > t_ms || !(v.tick > 0)) continue;
        if (best == nullptr || v.valid_from_ms > best->valid_from_ms ||
            (v.valid_from_ms == best->valid_from_ms && v.version > best->version))
            best = &v;
    }
    return best;
}

double round_to_tick(double price, double tick) {
    const double t = tick > 0 ? tick : derive_tick(price);
    if (t <= 0) return price;
    return std::round(price / t) * t;
}

FillEstimate estimate_fill(EntryType type, double distance_bps, double spread_bps,
                           double top_depth_usdt, double trade_intensity, double volatility_bps,
                           double liquidity_score) {
    FillEstimate f;
    // Market / stop-market entries fill near-immediately once triggered.
    if (type == EntryType::market) {
        f.p50_min = 0.02;
        f.p90_min = 0.1;
        f.timeout_ms = 60'000;
        return f;
    }
    // Passive fill time grows with distance-to-market and shrinks with trade
    // intensity, depth and liquidity; volatility helps a resting order get hit.
    const double dist = std::max(distance_bps, 0.0);
    const double intensity = std::max(trade_intensity, 0.1);   // trades/min proxy
    const double vol = std::max(volatility_bps, 1.0);
    const double depth_factor = std::clamp(50000.0 / std::max(top_depth_usdt, 1000.0), 0.3, 3.0);
    // A [0,1] score, 1 the best book (DecisionContext::liquidity_score). It was
    // fed AltTech's ORDINAL rank 1..4, which put every venue but the best on
    // the 0.4 floor -- a worse book estimated a FASTER fill than the best one.
    const double liq_factor = std::clamp(1.5 - liquidity_score, 0.4, 2.0);
    // Base minutes: distance covered by volatility drift + queue wait.
    double base = (dist / vol) * 3.0 + spread_bps / std::max(intensity, 1.0);
    base *= depth_factor * liq_factor;
    if (type == EntryType::stop_market || type == EntryType::stop_limit) base *= 0.6;
    f.p50_min = std::clamp(base, 0.05, 240.0);
    f.p90_min = std::clamp(base * 3.0, 0.2, 720.0);
    f.timeout_ms = static_cast<int64_t>(std::clamp(f.p90_min * 2.0, 1.0, 240.0) * 60'000);
    return f;
}

std::vector<TradePlanVariant> generate_plans(const StrategyCandidate& c, const DecisionContext& ctx,
                                             int64_t now_ms, const PlanConfig& cfg) {
    for (const auto limit : {cfg.scalp_max_ms, cfg.intraday_max_ms, cfg.extended_max_ms}) {
        if (limit < 5LL * 60 * 1000 || limit > kMaxHoldingMs)
            throw std::invalid_argument("holding limit must be in [5 minutes,48 hours]");
    }
    std::vector<TradePlanVariant> out;
    const double px = ctx.last_price() > 0 ? ctx.last_price() : c.entry_hint;
    const double atr = ctx.atr() > 0 ? ctx.atr() : px * 0.004;
    if (px <= 0 || atr <= 0 || c.side == Side::unspecified) return out;
    const bool is_long = c.side == Side::long_;
    const double dir = is_long ? 1.0 : -1.0;

    // The grid every price is placed on: the venue's at T0, else a configured
    // one, else a guess from the price magnitude that the plan records as one.
    const double tick = ctx.price_tick > 0 ? ctx.price_tick : cfg.tick_size > 0 ? cfg.tick_size : derive_tick(px);
    const char* tick_source = ctx.price_tick > 0 ? "instrument_spec" : cfg.tick_size > 0 ? "config" : "derived";

    // The model's expected excursions FOR THIS SIDE over the candidate's own
    // horizon, in price units (QUALITY-01/02). Only an ACTIVE, fresh, complete
    // prediction shapes a plan; otherwise both stay 0 and the geometry comes
    // from structure, the strategy's own levels and ATR, as it says.
    double ml_mae = 0, ml_mfe_dist = 0;
    if (const auto* h = geometry_prediction(c)) {
        if (const auto ex = side_excursions(*h, c.side)) {
            ml_mfe_dist = units::bps_to_price_distance(px, ex->favourable_bps);
            ml_mae = units::bps_to_price_distance(px, ex->adverse_bps);
        }
    }

    // Base stop distance from the candidate invalidation, floored by ATR/spread/
    // slippage buffers and by ML MAE (§15). Never one fixed percent for all pairs.
    const double inval_dist = std::abs(c.entry_hint - c.invalidation_hint);
    const double spread_slip = units::bps_to_price_distance(px, ctx.spread_bps + ctx.expected_slippage_bps);
    const double min_stop = std::max(units::bps_to_price_distance(px, cfg.min_stop_bps), spread_slip * 1.5);
    const double base_stop_dist = std::max({inval_dist, atr * 0.8, min_stop, ml_mae});
    // Which floor set it, first in this order on a tie.
    const char* stop_basis = base_stop_dist == inval_dist ? "strategy_invalidation"
                             : base_stop_dist == atr * 0.8 ? "atr_buffer"
                             : base_stop_dist == ml_mae    ? "ml_mae"
                                                           : "min_stop_floor";

    // Base target: structure zone in direction, else ML MFE, else the
    // strategy's own target level, else a multiple of the stop.
    const std::string tgt_kind = is_long ? "resistance" : "support";
    double struct_target = 0;
    if (const auto* z = ctx.nearest_zone(tgt_kind, px)) struct_target = z->center;
    const double hint_target_dist = std::abs(c.target_hint - c.entry_hint);

    struct Profile {
        PlanProfile profile;
        double entry_offset_atr;   // signed: negative = closer to market for the trade direction
        double stop_mult;
        double target_mult;
        EntryType forced_entry;    // unspecified => choose
        double fill_bias;
    };
    const EntryType chosen = choose_entry(c, ctx);

    // A pullback entry FADES the signal: the limit sits on the favourable side,
    // so it fills only once price has moved AGAINST the trade. For a
    // mean-reversion, VWAP-reclaim or crowding-unwind setup that is the thesis —
    // you are buying the dip on purpose. For a CONTINUATION setup it inverts the
    // thesis: momentum_continuation says "the trend is up, buy", and a limit
    // 0.3-0.8 ATR below market only fills after that trend has already broken.
    //
    // Measured over 12 h on live outcomes, against the random-walk baseline
    // implied by each variant's own R:R (target/(target+stop)):
    //
    //     limit  long   24.0 % vs 43.4 % random    market long   18.6 % vs 30.5 %
    //     limit  short  32.8 % vs 42.1 % random    market short  32.0 % vs 33.4 %
    //
    // Market-entry SHORT sits essentially at its random baseline, which is the
    // control proving the outcome reconstruction is unbiased. On the same side,
    // the limit variant runs 7.5-9.3 points of win rate BELOW the market
    // variant — that gap is the adverse selection, and it is paid on every
    // continuation trade the engine takes.
    const bool continuation = c.strategy_family == Family::trend ||
                              c.strategy_family == Family::breakout;
    const double fade = continuation ? 0.0 : 1.0;
    const std::vector<Profile> profiles = {
        {PlanProfile::aggressive, 0.0, 0.85, 1.5, chosen == EntryType::limit ? EntryType::market : chosen, 0.0},
        {PlanProfile::balanced, 0.3 * fade, 1.0, 1.15, chosen, 0.3},
        {PlanProfile::conservative, 0.8 * fade, 1.35, 0.85,
         continuation ? EntryType::market : EntryType::limit, 0.8},
    };

    for (const auto& pr : profiles) {
        TradePlanVariant v;
        v.candidate_id = c.candidate_id;
        v.context_id = c.context_id;
        v.canonical_pair_id = c.canonical_pair_id;
        v.profile = pr.profile;
        v.side = c.side;
        v.holding_class = c.holding_class;
        v.entry_type = pr.forced_entry;

        // Entry: aggressive at/near market; balanced small pullback; conservative
        // deeper pullback (limit further from market against the trade direction).
        double entry = px - dir * pr.entry_offset_atr * atr;
        if (pr.forced_entry == EntryType::market) entry = px;
        v.tick_size = tick;
        v.tick_source = tick_source;
        v.geometry_version = kPlanGeometryVersion;
        v.entry_price = round_to_tick(entry, tick);
        if (pr.forced_entry == EntryType::limit || pr.forced_entry == EntryType::stop_limit) {
            const double halfzone = atr * 0.1;
            v.entry_lower = round_to_tick(entry - halfzone, tick);
            v.entry_upper = round_to_tick(entry + halfzone, tick);
        }
        if (pr.forced_entry == EntryType::stop_market || pr.forced_entry == EntryType::stop_limit) {
            // Trigger beyond the market in the trade direction (breakout confirm).
            v.trigger_price = round_to_tick(px + dir * atr * 0.15, tick);
            v.entry_price = v.trigger_price;
        }

        // Stop.
        const double stop_dist = base_stop_dist * pr.stop_mult;
        v.stop_loss_price = v.entry_price - dir * static_cast<double>(stop_ticks(stop_dist, tick)) * tick;
        v.stop_basis = stop_basis;
        v.invalidation_reason = c.reason_codes.empty() ? "" : c.reason_codes.front();

        // Target: prefer structure, then ML MFE, then the strategy's target,
        // then a multiple of the stop distance. The basis names the source
        // actually used: a strategy target used to be recorded as rr_target.
        double target_dist = stop_dist * (pr.target_mult / pr.stop_mult) * 1.6;
        std::string tbasis = "rr_target";
        if (struct_target > 0) {
            const double sd = std::abs(struct_target - v.entry_price);
            if (sd > stop_dist * 0.8) { target_dist = sd * (pr.profile == PlanProfile::conservative ? 0.85 : 1.0); tbasis = "structure_zone"; }
        }
        if (ml_mfe_dist > 0 && tbasis == "rr_target") { target_dist = ml_mfe_dist * pr.target_mult; tbasis = "ml_mfe"; }
        if (hint_target_dist > 0 && tbasis == "rr_target") { target_dist = hint_target_dist * pr.target_mult; tbasis = "strategy_target"; }
        // Price-side invariants (§13) hold by construction: both levels are at
        // least one tick from the entry, on its side. A target that rounds to
        // a single tick is reported as such by the distances below -- and by
        // the proposal's cost diagnostic -- never widened.
        v.take_profit_price = v.entry_price + dir * static_cast<double>(target_ticks(target_dist, tick)) * tick;
        v.target_basis = tbasis;

        // Distances describe the final prices.
        v.stop_distance_bps = units::price_distance_to_bps(v.entry_price, v.entry_price - v.stop_loss_price);
        v.stop_distance_atr = atr > 0 ? std::abs(v.entry_price - v.stop_loss_price) / atr : 0;
        v.target_distance_bps = units::price_distance_to_bps(v.entry_price, v.take_profit_price - v.entry_price);
        const double risk = std::abs(v.entry_price - v.stop_loss_price);
        v.expected_rr = risk > 0 ? std::abs(v.take_profit_price - v.entry_price) / risk : 0;

        // Fill estimate + entry timeout.
        const double dist_bps = units::price_distance_to_bps(px, v.entry_price - px);
        const double vol_bps = units::price_distance_to_bps(px, atr);
        // A fill estimate has to produce a number -- the entry timeout depends on
        // it -- so this is a deliberate conservative stand-in, not an observation.
        // Intensity divides the spread term in estimate_fill(), so a LOW value
        // lengthens the estimated fill and the timeout. The old default of 5.0
        // was optimistic: with no tape at all it claimed a briskly trading
        // market. 1.0 trade/min is the pessimistic end of the same scale.
        constexpr double kUnobservedTradeIntensity = 1.0;
        const double intensity =
            ctx.microstructure.get("trade_intensity", kUnobservedTradeIntensity);
        const FillEstimate fe = estimate_fill(v.entry_type, dist_bps, ctx.spread_bps,
                                              ctx.top_depth_usdt, intensity, vol_bps,
                                              // Absent: the midpoint, as the slippage term charges.
                                              ctx.liquidity_score().value_or(0.5));
        v.expected_fill_time_p50_min = fe.p50_min;
        v.expected_fill_time_p90_min = fe.p90_min;
        // Time model (§14): max holding capped by class and the hard 48h invariant.
        // Bounded by the absolute ceiling as well as the configured cap, so a
        // PlanConfig built in code (tests, replay, a future caller) cannot
        // exceed it either.
        v.max_holding_time_ms = std::min({holding_cap(c.holding_class, cfg),
                                          cfg.extended_max_ms, kMaxHoldingMs});
        // An entry later than the hypothesis' own holding window is no entry
        // into it. The fill estimate (up to 4 h for a passive order in a thin
        // book) exceeded the 20-45 min window of the scalp setups, and the
        // plan's validity check then refused the whole signal as
        // ENTRY_TIMEOUT_EXCEEDS_HOLDING -- a market event lost to a plan
        // parameter (12 % of published signals, 2026-09-26).
        v.entry_fill_timeout_ms = std::min({fe.timeout_ms, cfg.max_entry_timeout_ms, v.max_holding_time_ms});
        v.post_only_allowed = v.entry_type == EntryType::limit;
        v.max_slippage_bps = v.entry_type == EntryType::market ? cfg.default_max_slippage_bps * 1.5
                                                               : cfg.default_max_slippage_bps;

        v.time_stop_at_ms = now_ms + v.max_holding_time_ms;

        v.natural_key = ids::join({c.candidate_id, to_string(pr.profile)});
        v.variant_id = ids::make("plan", v.natural_key);
        out.push_back(std::move(v));
    }
    return out;
}

} // namespace altstrategy::engine
