#pragma once

// altstrategy.v1 decision-engine domain — pure C++ value types (no IO, no proto).
//
// The whole engine (context assembly, catalogue, ensemble, allocator, ranking,
// plan, proposal, outcomes, attribution, adaptive state) operates on these
// structs. Proto and PostgreSQL conversions live only at the IO boundary
// (wire_codec, decision_store), so all decision logic is unit-testable in
// isolation. Enum ⇄ string helpers give a single canonical spelling shared by
// the wire, the database and the logs.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace altstrategy::engine {

// The observation universe is the four-venue intersection, so AltTech's
// `bitget_liquidity_rank` is an ORDINAL over 1..4 -- 1 the tightest spread.
inline constexpr double kVenueUniverseSize = 4.0;

// ------------------------------ enums ---------------------------------------

enum class Side { unspecified, long_, short_ };

// Hypothesis families. The ensemble measures independence across these, so a
// family is a claim about MECHANISM, not about which indicator was read: two
// strategies in one family are expected to be right and wrong together.
enum class Family {
    unspecified, trend, breakout, mean_reversion, cross_venue, vwap, crowding,
    microstructure,   // resting-book pressure
    order_flow,       // executed-flow / absorption
    basis,            // funding and mark-index carry
    liquidity,        // spread and depth dislocation
};

enum class Lifecycle {
    unspecified, research, backtested, shadow, candidate, limited, active, degraded, disabled, retired
};

// The absolute holding ceiling for every strategy, candidate, plan and signal.
//
// This is a SAFETY CEILING, not a normal holding period: the system is a
// short-term engine and most hypotheses should close far sooner. It lived only
// inside PlanConfig as `extended_max_ms`, applied with a std::min() at plan
// construction -- so a strategy declaring a 100-hour horizon was silently
// truncated to 48 rather than refused, and nothing upstream of the plan knew
// the horizon had been overruled. Silent truncation turns a strategy's stated
// thesis into a different one without saying so.
inline constexpr int64_t kMaxHoldingMs = 48LL * 3600 * 1000;

// The AltML prediction contract this engine speaks, and the venue a prediction
// must be ABOUT. Both travel on every PredictionV1 and both are refused when
// they disagree: field meanings are only stable within a schema version, and a
// prediction about an information venue is not evidence about the one traded.
// 2: AltML econ models. Schema 1 (a binary direction probability) is no longer
// consumed: the decision policy needs the econ estimate, and reading a schema-1
// probability as one would be the silent semantic substitution the policy
// exists to prevent.
// 3: + the versioned cost model by component, the model's validated policy
// margin, and favourable/adverse magnitudes per side. Schema 2 is still read,
// without a margin, and is therefore never actionable.
inline constexpr int32_t kPredictionSchemaVersion = 4;

enum class HoldingClass { unspecified, scalp, intraday, extended_intraday };

enum class Grade { unspecified, strong, normal, weak, experimental, shadow, invalid };

enum class EntryType { unspecified, market, limit, stop_market, stop_limit };

enum class SelectionMode { unspecified, exploit, explore_shadow };

enum class PlanProfile { unspecified, aggressive, balanced, conservative };

enum class ProposalState {
    unspecified, draft, context_ready, candidates_generated, ranked, plans_generated,
    ml_evaluated, proposed, superseded, expired, cancelled, entry_expired,
    outcome_pending, outcome_final
};

enum class RankingResult {
    unspecified, recommended, multiple_close, conflicted, no_clear_winner, no_candidates
};

enum class MarketOutcomeType {
    unspecified, entry_not_reached, entry_expired, target_first, stop_first,
    time_stop, cancelled, ambiguous, data_incomplete
};

enum class OutcomePath { unspecified, raw_trades, s1_trade_agg, s1_price_book_agg, m1_candles };

enum class DriftStatus { unspecified, stable, watch, degraded, structural_break };

enum class ContextQuality { unspecified, strict_4_of_4, degraded_3_of_4, degraded_2_of_4, invalid };

// ------------------------------ enum <-> string -----------------------------

[[nodiscard]] std::string to_string(Side s);
[[nodiscard]] Side side_from_string(std::string_view s);
[[nodiscard]] std::string to_string(Family f);
[[nodiscard]] Family family_from_string(std::string_view s);
[[nodiscard]] std::string to_string(Lifecycle l);
[[nodiscard]] Lifecycle lifecycle_from_string(std::string_view s);
[[nodiscard]] std::string to_string(HoldingClass h);
[[nodiscard]] HoldingClass holding_from_string(std::string_view s);
[[nodiscard]] std::string to_string(Grade g);
[[nodiscard]] Grade grade_from_string(std::string_view s);
[[nodiscard]] std::string to_string(EntryType e);
[[nodiscard]] EntryType entry_from_string(std::string_view s);
[[nodiscard]] std::string to_string(SelectionMode m);
[[nodiscard]] std::string to_string(PlanProfile p);
[[nodiscard]] std::string to_string(ProposalState s);
[[nodiscard]] std::string to_string(RankingResult r);
[[nodiscard]] std::string to_string(MarketOutcomeType o);
[[nodiscard]] MarketOutcomeType market_outcome_from_string(std::string_view s);
[[nodiscard]] std::string to_string(OutcomePath p);
[[nodiscard]] std::string to_string(DriftStatus d);
[[nodiscard]] std::string to_string(ContextQuality q);
[[nodiscard]] ContextQuality quality_from_string(std::string_view s);

[[nodiscard]] Side opposite(Side s);

// ------------------------------ DecisionContext (§7) ------------------------

struct FeatureSummary {
    std::map<std::string, double> values;
    int64_t bar_open_time_ms = 0;
    int64_t revision = 0;
    bool valid = false;
    // Set when the producer's vector length did not match the configured
    // ordered name list, so no value could be attached to a name safely.
    bool schema_mismatch = false;
    size_t producer_value_count = 0;

    [[nodiscard]] double get(const std::string& k, double def = 0.0) const {
        const auto it = values.find(k);
        return it == values.end() ? def : it->second;
    }
    [[nodiscard]] bool has(const std::string& k) const { return values.count(k) > 0; }
};

struct RegimeView {
    std::string regime = "UNDEFINED";
    double confidence = 0;
    double trend_strength = 0;
    double volatility_regime = 0;
    double liquidity_quality = 0;
    int64_t bar_open_time_ms = 0;
};

struct ProjectedZone {
    int64_t zone_id = 0;
    std::string kind;      // support | resistance | dynamic
    double lower = 0, center = 0, upper = 0;
    double strength = 0, confidence = 0;
    std::string lifecycle;
};

// What AltML actually produces. See docs/PREDICTION_CONTRACT.md for the field
// inventory this was cut down to.
//
// It previously declared p_flat, three net-return quantiles, three MFE and
// three MAE quantiles, a volatility quantile, an OOD score and a quality route.
// AltML computes none of them. They were not harmlessly unused: a double nobody
// assigns reads as 0, so `ood_score = 0` asserted "this observation is not out
// of distribution" -- an all-clear derived from nothing. Fields without a
// producer are removed rather than zero-filled.
struct MarketForecastView {
    bool present = false;   // a prediction was received and is fresh
    bool abstain = false;   // the model declined; probabilities are meaningless

    double p_up = 0, p_down = 0;
    double expected_return = 0;              // point estimate, after-fee basis
    double probability_target_before_stop = 0;
    double expected_mfe = 0, expected_mae = 0;
    double expected_holding_time_min = 0;

    // NOT an uncertainty estimate. AltML defines it as |p_up - 0.5| * 2, a
    // deterministic transform of the probability, so it says how far from a
    // coin-flip the model landed and nothing about how sure it is.
    double conviction = 0;

    std::string quality_status;              // VALID | DEGRADED
    std::string model_id;
    int32_t model_version = 0;
    std::string feature_set_version;
    int32_t prediction_schema_version = 0;

    int64_t bar_open_time_ms = 0;            // the T0 anchor
    int64_t bar_revision = 0;
    int64_t computed_at_ms = 0;
    std::string prediction_id;

    // --- schema 2: an econ model (one per horizon) --------------------------
    // SHORT / NEUTRAL / LONG over a fixed horizon against the all-in round
    // trip, temperature-calibrated. `expected_net_*_bps` is the model's
    // estimate of the MARKET outcome of a taker entry held for the horizon --
    // not of any plan's execution. Absent (econ = false) on anything older.
    bool econ = false;
    int32_t horizon_minutes = 0;
    std::string objective;
    double p_long = 0, p_neutral = 0, p_short = 0;
    double expected_net_long_bps = 0, expected_net_short_bps = 0;
    double round_trip_cost_bps = 0;
    std::string calibrator;
    std::string recipe_version;
    int32_t aligned_missing = 0;

    // --- schema 3: versioned cost model and policy, magnitudes ---------------
    // The cost the estimate was netted with, by component: fee (both legs,
    // taker), spread OBSERVED at T0 (a full spread: half paid entering, half
    // leaving), impact (an explicit assumption). spread_observed = false means
    // no book reading existed and the stated fallback was used.
    std::string cost_model_version;
    double fee_bps = 0, spread_bps = 0, impact_bps = 0;
    bool spread_observed = false;
    // The economic margin above cost the model's policy requires, chosen on
    // the model's VALIDATION slice (nested; never on its holdout). Absent: no
    // margin was validated, and no estimate from this model is actionable.
    std::optional<double> policy_margin_bps;
    std::string policy_version;
    // Expected favourable / adverse excursion over the horizon, per side, in
    // bps of the entry price (adverse <= 0). Magnitudes, not probabilities.
    double favorable_long_bps = 0, adverse_long_bps = 0;
    double favorable_short_bps = 0, adverse_short_bps = 0;

    // --- schema 4: market-prediction semantics -----------------------------
    // Whether the class probabilities passed AltML's holdout calibration test
    // (a raw score that did not is published, and labelled, as uncalibrated);
    // what "up / flat / down" means for this model; the expected excursions
    // as magnitudes (>= 0) and where they come from; the model's role.
    bool probability_calibrated = false;
    std::string calibration_version;
    std::string label_definition;
    bool has_excursions = false;
    double expected_up_excursion_bps = 0, expected_down_excursion_bps = 0;
    std::string magnitude_source;
    std::string model_role;
};

// ONE horizon's market prediction, as a VPS1 signal carries it.
//
// VPS1 PREDICTS the market; it does not decide whether acting pays. So every
// signal carries AltML's view on every horizon it serves (15m / 1h / 4h), in
// market terms: the probabilities of an up / flat / down outcome, the
// expected return and the expected up and down excursions over the horizon,
// all in the LONG perspective (the signal's side is on the signal). Whether
// that move is large enough for today's costs is VPS2's question.
//
// `state` says whether numbers exist and in what condition:
//   PRESENT       fresh, every input the model reads was present
//   INCOMPLETE    fresh, but some aligned inputs were missing (degraded)
//   STALE         older than the freshness bound
//   ABSTAIN       the model declined
//   INCOMPATIBLE  a forecast arrived that this build cannot read
//   MISSING       nothing for this horizon
// Numbers are meaningful only when has_values() -- never a 0 standing in.
enum class PredictionState { missing, present, incomplete, stale, abstain, incompatible };
[[nodiscard]] std::string to_string(PredictionState s);

struct HorizonPrediction {
    int32_t horizon_minutes = 0;
    PredictionState state = PredictionState::missing;
    std::string model_id;
    int32_t model_version = 0;
    // ACTIVE: the model passed AltML's market-quality gates. SHADOW: it did
    // not (or has not yet); its numbers are carried and labelled as such.
    std::string role;
    std::string objective;
    std::string calibrator;
    bool probability_calibrated = false;
    double p_up = 0, p_flat = 0, p_down = 0;
    double expected_return_bps = 0;
    double expected_up_excursion_bps = 0;     // >= 0: how far above the reference
    double expected_down_excursion_bps = 0;   // >= 0: how far below the reference
    int64_t bar_open_time_ms = 0;
    int64_t computed_at_ms = 0;
    int32_t aligned_missing = 0;
    // Economic DIAGNOSTIC (AltML's cost model at T0): recorded, never a gate.
    std::string cost_model_version;
    double cost_bps = 0;
    double expected_net_long_bps = 0, expected_net_short_bps = 0;
    [[nodiscard]] bool has_values() const {
        return state == PredictionState::present || state == PredictionState::incomplete ||
               state == PredictionState::stale;
    }
};

// What the economic policy concluded about ONE candidate. Every state is a
// separate outcome so that "no setup", "setup but the model says it does not
// pay" and "setup but no usable model" never collapse into one silence.
//
// The edge states are defined on the calibrated expected net E (this side,
// after the estimated cost C) and the model's validated margin M:
//   NO_EDGE        E <= 0
//   SMALL_EDGE     0 < E <= M, or no validated M exists   (positive, not enough)
//   POSITIVE_EDGE  M < E < M + C                          (actionable)
//   STRONG_EDGE    E >= M + C: the move is expected to pay its costs twice over
// "Positive" therefore never means "> 0 by rounding".
enum class EconState {
    no_data,            // the model scored with inputs it reads missing (aligned blocks)
    model_unavailable,  // no usable estimate for the horizon (none, or incompatible)
    model_stale,        // an estimate older than the freshness bound
    model_abstain,      // the model declined
    model_shadow,       // only a SHADOW model answered: recorded, never acted on
    horizon_conflict,   // another horizon's model has an edge the OTHER way
    no_edge,
    small_edge,
    positive_edge,
    strong_edge
};
[[nodiscard]] std::string to_string(EconState s);

struct EconomicDecision {
    EconState state = EconState::model_unavailable;
    int32_t horizon_minutes = 0;            // the horizon evaluated
    double p_side = 0, p_opposite = 0, p_neutral = 0;
    double expected_net_bps = 0;            // for THIS side, cost included
    double expected_gross_bps = 0;          // for THIS side
    double round_trip_cost_bps = 0;
    std::string model_id;
    int64_t forecast_bar_open_ms = 0;
    std::optional<double> margin_bps;       // the validated margin applied, if any
    std::string reason_code;                // POSITIVE_EDGE, NO_EDGE, MODEL_STALE, ...
    [[nodiscard]] bool actionable() const {
        return state == EconState::positive_edge || state == EconState::strong_edge;
    }
    [[nodiscard]] bool evaluated() const {
        return state == EconState::positive_edge || state == EconState::strong_edge ||
               state == EconState::small_edge || state == EconState::no_edge ||
               state == EconState::horizon_conflict;
    }
    // Numbers exist (an ACTIVE or a SHADOW model answered) -- what is persisted
    // for learning. Only evaluated() estimates rank, only actionable() ones
    // recommend.
    [[nodiscard]] bool has_estimate() const {
        return evaluated() || state == EconState::model_shadow || state == EconState::no_data;
    }
};

struct DecisionContext {
    std::string context_id;
    int64_t canonical_pair_id = 0;
    int64_t bitget_instrument_id = 0;
    int64_t event_time_ms = 0;
    int64_t available_at_ms = 0;
    int64_t expires_at_ms = 0;
    int64_t context_revision = 0;

    std::string global_universe_status = "ELIGIBLE_ALL_VENUES";
    ContextQuality data_quality = ContextQuality::strict_4_of_4;
    uint32_t venue_mask = 0;
    uint32_t missing_venue_mask = 0;
    bool regime_divergence = false;

    FeatureSummary bitget_features;          // the EXECUTION timeframe
    // Higher-timeframe context for the target venue, keyed by bar length in
    // minutes (5, 15, 30, 60, 240, 1440). Separate from the execution grid on
    // purpose: a stop sized from a daily ATR and one sized from a 5-minute ATR
    // are different decisions, and the assembler used to keep whichever vector
    // arrived last. A grid with no valid observation at T0 is simply absent --
    // never substituted from the execution timeframe.
    std::map<int32_t, FeatureSummary> context_features;
    // One summary PER information venue, keyed by AltTech's exchange id
    // (2 Binance, 3 OKX, 4 Bybit). A single flat summary would have merged three
    // markets into one reading, which is the mistake this whole layer exists to
    // avoid: BINANCE.RSI_14 and BYBIT.RSI_14 are different facts about different
    // books. Absent venue = absent key, never a zero-filled entry.
    std::map<int32_t, FeatureSummary> reference_features;
    FeatureSummary cross_venue_features;
    FeatureSummary microstructure;

    RegimeView bitget_regime;      // Bitget 1m venue_lite regime
    RegimeView global_regime;
    // Bitget's own 15m and 1h regime (AltTech regime_module), keyed by bar
    // length in minutes. The 1m regime above and these are different
    // statements about different horizons; a setup on the 15m grid asks for
    // the trend on its own or a higher grid, never the 1m one. A grid whose
    // regime is not fresh is absent.
    std::map<int32_t, RegimeView> bitget_htf_regime;

    std::vector<ProjectedZone> bitget_projected_zones;
    std::string bitget_structure;
    std::string consensus_structure;

    MarketForecastView market_forecast;
    // Every fresh econ forecast for this pair, keyed by horizon in minutes.
    std::map<int32_t, MarketForecastView> forecasts;

    double fees_bps = 4.0;
    double funding_rate = 0;
    double mark_price = 0;
    double index_price = 0;
    double spread_bps = 0;
    double top_depth_usdt = 0;
    double liquidity_rank = 0;
    double expected_slippage_bps = 0;
    // Whether the two above are MEASUREMENTS or the conservative stand-ins used
    // when the book is unavailable. A fallback is a legitimate way to keep
    // planning conservative; presenting it as an observed spread is not, and
    // for a long time nothing downstream could tell the difference.
    bool spread_observed = false;
    bool depth_observed = false;

    std::string feature_manifest_hash;
    std::map<std::string, std::string> producer_versions;
    std::string symbol;

    // Reference price for strategy geometry. The TA-Lib feature set carries no
    // price member, so the price comes from the market: the cross-venue
    // consensus mid (assembled into mark_price), then the venue's own book mid
    // from the live microstructure window, then the index.
    [[nodiscard]] double last_price() const {
        if (mark_price > 0) return mark_price;
        // AltTech's bitget_micro v1 publishes no mid -- position 24 is
        // book_mid_RETURN, which must never land here -- so this branch is
        // inert against today's producer and is kept only for a deployment
        // whose microstructure set does expose one. In production the price is
        // the cross-venue consensus mark.
        const double mid = microstructure.get("book_mid", 0);
        if (mid > 0) return mid;
        const double vwap = microstructure.get("vwap", 0);
        return vwap > 0 ? vwap : index_price;
    }
    // Bitget's liquidity among the venues as a score on [0,1], 1 the best: the
    // ordinal rank mapped linearly, the same normalisation the expected-
    // slippage impact term uses. Absent (rank 0) is UNKNOWN, not good.
    [[nodiscard]] std::optional<double> liquidity_score() const {
        if (!(liquidity_rank >= 1.0)) return std::nullopt;
        const double s = 1.0 - (liquidity_rank - 1.0) / (kVenueUniverseSize - 1.0);
        return s < 0.0 ? 0.0 : (s > 1.0 ? 1.0 : s);
    }
    // ATR(14) in PRICE units, as TA-Lib reports it. This must never fall back to
    // a value from a different feature set: a stop is sized in multiples of it,
    // so a wrong unit here is a wrong stop on every trade.
    [[nodiscard]] double atr() const { return bitget_features.get("atr_14", 0); }
    // Nearest projected zone of a kind relative to price.
    [[nodiscard]] const ProjectedZone* nearest_zone(const std::string& kind, double price) const;
};

// AltML CandidateEvaluation result (§11/§23), or a deterministic fallback when
// AltML is unavailable (unavailable=true → the proposal carries ML_UNAVAILABLE).
struct MlEvaluation {
    bool present = false;
    bool abstain = false;
    bool unavailable = false;

    // Straight from AltML's PredictionV1. Fields it does not compute are not
    // declared here: p_entry_reached, p_timeout, p_ambiguous, expected_utility,
    // MFE/MAE quantiles, an uncertainty score and an OOD score were all carried
    // as zeros nobody set, and a zero for `ood` asserts "in distribution".
    double p_target_before_stop = 0;
    double expected_net_return = 0;          // after-fee basis, per AltML
    double expected_mfe = 0, expected_mae = 0;
    double expected_holding_time_min = 0;
    // |p_up - 0.5| * 2. Distance from a coin flip, NOT an uncertainty estimate.
    double conviction = 0;
    double p_up = 0, p_down = 0;
    std::string quality_status;
    std::vector<std::string> model_versions;
};

// ------------------------------ StrategyCandidate (§8.1) --------------------

struct StrategyCandidate {
    std::string candidate_id;
    std::string natural_key;
    int64_t canonical_pair_id = 0;
    std::string strategy_id;
    std::string strategy_version;
    std::string parameter_set_id = "default";
    Family strategy_family = Family::unspecified;
    Side side = Side::unspecified;
    int64_t setup_time_ms = 0;
    int64_t created_at_ms = 0;
    int64_t expires_at_ms = 0;
    std::string context_id;
    int64_t context_revision = 0;

    double raw_setup_score = 0;
    double static_score = 0;
    double adaptive_score = 0;
    double strategy_prior = 0;
    double strategy_posterior = 0;
    double posterior_uncertainty = 0;
    double effective_sample_size = 0;

    int64_t expected_horizon_ms = 0;
    HoldingClass holding_class = HoldingClass::intraday;
    double entry_hint = 0;
    double invalidation_hint = 0;
    double target_hint = 0;

    std::string bitget_regime;
    std::string global_regime;
    double cross_venue_confirmation = 0;

    MarketForecastView ml_context;
    // AltML's market prediction on every served horizon, attached to every
    // candidate whatever its verdict (signal_predictions.hpp).
    std::vector<HorizonPrediction> predictions;
    // Measured description of the setup (strategy.hpp RawSignal::descriptors).
    std::map<std::string, double> descriptors;
    // The economic DIAGNOSTIC for this candidate (economic_policy.hpp): what
    // the model's expected move is worth after AltML's cost estimate. It gates
    // nothing in VPS1.
    EconomicDecision econ;
    ContextQuality quality = ContextQuality::strict_4_of_4;
    std::vector<std::string> warnings;
    std::vector<std::string> reason_codes;
    SelectionMode selection_mode = SelectionMode::exploit;
    Lifecycle lifecycle_status = Lifecycle::active;

    // Raw reward/risk from the hints; 0 when geometry is degenerate/inverted.
    [[nodiscard]] double raw_rr() const;
};

// ------------------------------ Ensemble (§15) ------------------------------

struct EnsembleView {
    Side ensemble_direction = Side::unspecified;
    double ensemble_agreement = 0;
    std::vector<std::string> supporting_strategies;
    std::vector<Family> supporting_families;
    std::vector<std::string> conflicting_strategies;
    std::vector<Family> conflicting_families;
    double family_independence_score = 0;
    double plan_dispersion = 0;
    std::string ensemble_id;
};

// ------------------------------ Ranking (§8.2, §19) -------------------------

struct RankedCandidate {
    std::string candidate_id;
    std::string strategy_id;
    Family strategy_family = Family::unspecified;
    Side side = Side::unspecified;
    double final_score = 0;
    double adaptive_weight = 0;
    double posterior_edge = 0;
    int rank = 0;
    SelectionMode selection_mode = SelectionMode::exploit;
    std::map<std::string, double> components;
};

struct RankedProposal {
    std::string ranking_id;
    std::string natural_key;
    int64_t canonical_pair_id = 0;
    std::string context_id;
    std::string ranking_policy_id;
    std::string ranking_policy_version;
    int64_t created_at_ms = 0;
    std::vector<RankedCandidate> ranked_candidates;
    EnsembleView ensemble;
    double ranking_uncertainty = 0;
    double ranking_regret_estimate = 0;
    std::string recommended_candidate_id;
    RankingResult result = RankingResult::no_candidates;
    bool is_challenger = false;
};

// ------------------------------ Trade plan (§22) ----------------------------

struct TradePlanVariant {
    std::string variant_id;
    std::string natural_key;
    std::string candidate_id;
    std::string context_id;
    int64_t canonical_pair_id = 0;
    PlanProfile profile = PlanProfile::balanced;
    Side side = Side::unspecified;
    EntryType entry_type = EntryType::limit;
    double entry_price = 0;
    double entry_lower = 0;
    double entry_upper = 0;
    double trigger_price = 0;
    double stop_loss_price = 0;
    double take_profit_price = 0;
    double optional_secondary_target = 0;
    int64_t max_holding_time_ms = 0;
    int64_t time_stop_at_ms = 0;
    double expected_fill_time_p50_min = 0;
    double expected_fill_time_p90_min = 0;
    int64_t entry_fill_timeout_ms = 0;
    double stop_distance_bps = 0;
    double stop_distance_atr = 0;
    double target_distance_bps = 0;
    double expected_rr = 0;
    std::string stop_basis;
    std::string target_basis;
    std::string invalidation_reason;
    bool post_only_allowed = false;
    double max_slippage_bps = 0;
    HoldingClass holding_class = HoldingClass::intraday;
};

// ------------------------------ Proposal (§8.3, §9) -------------------------

struct ProposalWarning {
    std::string code;
    std::string detail;
    std::string severity = "warning"; // info | warning | risk_flag
};

struct TradeSignalProposal {
    std::string signal_proposal_id;
    std::string natural_key;
    std::string candidate_id;
    std::string ranking_id;
    int64_t canonical_pair_id = 0;
    int64_t bitget_instrument_id = 0;
    std::string strategy_id;
    std::string strategy_version;
    std::string parameter_set_id = "default";
    Side side = Side::unspecified;
    Grade proposal_grade = Grade::normal;
    int plan_version = 1;
    HoldingClass holding_class = HoldingClass::intraday;
    int64_t created_at_ms = 0;
    int64_t valid_until_ms = 0;

    EntryType entry_type = EntryType::limit;
    double entry_price = 0;
    double entry_lower = 0;
    double entry_upper = 0;
    double trigger_price = 0;
    double max_slippage_bps = 0;
    bool post_only_allowed = false;
    double expected_fill_time_p50_min = 0;
    double expected_fill_time_p90_min = 0;
    int64_t entry_fill_timeout_ms = 0;

    double stop_loss_price = 0;
    std::string stop_basis;
    double stop_distance_bps = 0;
    double stop_distance_atr = 0;
    double take_profit_price = 0;
    double optional_secondary_target = 0;
    std::string target_basis;
    double target_distance_bps = 0;

    int64_t expected_holding_time_p50_ms = 0;
    int64_t expected_holding_time_p90_ms = 0;
    int64_t max_holding_time_ms = 0;
    int64_t time_stop_at_ms = 0;

    // Forward-looking economics. Present ONLY when a model produced them.
    //
    // `edge_estimated` is the honest half of this block. Without an active
    // AltML model the engine has no forecast, and it used to fill the gap with
    // invented constants: p_entry_reached = 0.99 or 0.7, p_timeout = 0.1,
    // p_ambiguous = 0.05, and a pair of "geometry-implied" probabilities
    // p = 1/(1+rr) and rr/(1+rr). Those two are fair odds, so
    // p_target*target - p_stop*stop is ALGEBRAICALLY ZERO for every rr -- the
    // expected gross return was not computed from anything, it was an identity
    // wearing the shape of an estimate, and every proposal reported net = -cost
    // as though that were a measurement. p_entry_reached, p_timeout,
    // p_ambiguous and expected_utility are removed outright: nothing produced
    // them and nothing read them.
    //
    // When edge_estimated is false these stay at 0 and are written as NULL --
    // absent, not zero.
    bool edge_estimated = false;
    // Whether p_target_before_stop was produced. The econ models do not
    // produce it: they estimate a fixed-horizon market outcome, not which of a
    // plan's barriers is touched first. Absent is written as NULL.
    bool p_target_estimated = false;
    double p_target_before_stop = 0;
    double expected_net_return = 0;
    // Reward/risk of the geometry. NOT an economic forecast: it is a property
    // of where the stop and target sit, always available, never a probability.
    double expected_rr = 0;
    // The cost this plan was priced with, decomposed and versioned, so the same
    // figure can score the trade when it resolves. See engine/cost_model.hpp.
    int cost_model_version = 0;
    double fee_bps_round_trip = 0;
    double spread_cost_bps = 0;
    double slippage_cost_bps = 0;
    double round_trip_cost_bps = 0;
    bool cost_spread_observed = false;

    // Conviction, not uncertainty: |p_up - 0.5| * 2 from AltML.
    double ml_conviction = 0;
    bool ml_abstain = false;
    double strategy_confidence = 0;
    double ensemble_agreement = 0;
    double adaptive_score = 0;

    ContextQuality quality = ContextQuality::strict_4_of_4;
    std::vector<ProposalWarning> warnings;
    std::vector<std::string> risk_flags;
    std::vector<std::string> reason_codes;
    bool recommended_for_execution = false;
    std::vector<std::string> alternative_plan_ids;

    int64_t context_revision = 0;
    std::string feature_manifest_hash;
    std::vector<std::string> model_versions;
    std::string ranking_policy_id;
    ProposalState state = ProposalState::proposed;
    std::string superseded_by_id;

    // VPS1 signal content (signal semantics vps1-signal-1): the market view
    // on every horizon and the setup's measured description.
    std::vector<HorizonPrediction> market_predictions;
    std::map<std::string, double> setup_descriptors;
    int32_t evaluation_horizon_minutes = 0;   // the horizon matching the setup's own
    std::string signal_semantics = "vps1-signal-1";
};

// ------------------------------ Learning loop -------------------------------

struct MarketOutcome {
    std::string outcome_id;
    std::string signal_proposal_id;
    std::string candidate_id;
    int64_t canonical_pair_id = 0;
    MarketOutcomeType outcome = MarketOutcomeType::unspecified;
    OutcomePath path_source = OutcomePath::m1_candles;
    int64_t entry_time_ms = 0;
    int64_t resolved_time_ms = 0;
    double entry_fill_price = 0;
    double exit_price = 0;
    double realized_gross_return = 0;
    double realized_net_return = 0;
    double mfe = 0;
    double mae = 0;
    int64_t holding_time_ms = 0;
    bool ambiguous = false;
    bool is_counterfactual = false;
    int64_t created_at_ms = 0;
    // Which cost definition produced realized_net_return, and the figure used.
    // Pooling outcomes across versions without saying so is how a measured edge
    // stops meaning anything -- see db/migrations/0005.
    int cost_model_version = 0;
    double round_trip_cost_bps = 0;
};

struct ExecutionFeedback {
    std::string signal_proposal_id;
    bool accepted = false;
    std::vector<std::string> rejection_reasons;
    bool order_submitted = false;
    std::string order_type;
    int64_t submitted_at_ms = 0;
    double fill_price = 0;
    int64_t fill_time_ms = 0;
    double fill_ratio = 0;
    double slippage_bps = 0;
    double fees = 0;
    double funding = 0;
    bool position_opened = false;
    bool position_closed = false;
    std::string stop_result;
    std::string target_result;
    std::string time_stop_result;
    double realized_pnl = 0;
    double mfe = 0;
    double mae = 0;
    std::vector<std::string> execution_anomalies;
    int64_t received_at_ms = 0;
};

struct CounterfactualOutcome {
    std::string counterfactual_id;
    std::string ranking_id;
    std::string selected_candidate_id;
    std::string candidate_id;
    // Which plan variant this row describes. The ranking yields one candidate,
    // so the real alternatives are the candidate's own geometry variants.
    int plan_version = 1;
    MarketOutcomeType outcome = MarketOutcomeType::unspecified;
    double realized_net_return = 0;
    double ranking_regret = 0;
    bool best_hindsight = false;
    int64_t created_at_ms = 0;
};

struct OutcomeAttribution {
    std::string attribution_id;
    std::string signal_proposal_id;
    double strategy_attribution = 0;
    double ranking_attribution = 0;
    double geometry_attribution = 0;
    double ml_attribution = 0;
    double execution_attribution = 0;
    double cost_attribution = 0;
    double regime_attribution = 0;
    std::string dominant_factor;
    int64_t created_at_ms = 0;
};

// ------------------------------ Adaptive state (§16-§18, §39, §40) ----------

struct StrategyPosterior {
    std::string strategy_id;
    std::string strategy_version;
    std::string parameter_set_id = "*";
    std::string regime = "*";
    std::string symbol_group = "*";
    int64_t canonical_pair_id = 0;
    Side side = Side::unspecified;
    HoldingClass holding_class = HoldingClass::unspecified;
    double strategy_prior = 0;
    double posterior_mean_edge = 0;
    double posterior_uncertainty = 1;
    double effective_sample_size = 0;
    double shrinkage_factor = 1;
    double time_decay_state = 0;
    double adaptive_weight = 1;
    int64_t sample_count = 0;
    int64_t updated_at_ms = 0;
};

struct StrategyDrift {
    std::string strategy_id;
    std::string strategy_version;
    DriftStatus status = DriftStatus::stable;
    double candidate_frequency_drift = 0;
    double score_distribution_drift = 0;
    double outcome_drift = 0;
    double fill_time_drift = 0;
    double holding_time_drift = 0;
    double mfe_mae_drift = 0;
    double regime_performance_drift = 0;
    double symbol_performance_drift = 0;
    std::vector<std::string> reasons;
    int64_t computed_at_ms = 0;
};

struct LifecycleTransition {
    std::string strategy_id;
    std::string strategy_version;
    Lifecycle from_status = Lifecycle::unspecified;
    Lifecycle to_status = Lifecycle::unspecified;
    std::string reason;
    int64_t sample_count = 0;
    bool rollback = false;
    std::string policy_version = "v1";
    int64_t created_at_ms = 0;
};

// The ML evaluation for a candidate, derived from the prediction already in the
// context. There is no request: AltML publishes, AltStrategy consumes.
//
// An absent or abstaining forecast yields unavailable/abstain -- never p = 0.5,
// never a carried-over previous prediction, never another timeframe's.
[[nodiscard]] MlEvaluation ml_from_forecast(const MarketForecastView& f);

} // namespace altstrategy::engine
