#pragma once

// wire codec: domain ⇄ altstrategy.v1 protobuf on the wire, plus parsing of the
// vendored upstream envelopes (altml.v1 forecasts/evaluations, alttech.v1
// analytics components) the context_assembler consumes. Keeps proto out of the
// pure engine core — only this IO layer links the generated code.

#include "altstrategy/engine/decision_types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace altstrategy::engine_io {

using namespace altstrategy::engine;

struct EnvelopeMeta {
    std::string message_id;
    std::string event_type;
    int64_t canonical_pair_id = 0;
    std::string symbol;
    std::string trace_id;
    std::string correlation_id;
};

// --- publish path: domain -> serialized altstrategy.v1.Envelope bytes ---
std::string encode_candidate(const StrategyCandidate& c, const EnvelopeMeta& m);
std::string encode_ensemble(const EnsembleView& e, int64_t pair, const EnvelopeMeta& m);
std::string encode_ranking(const RankedProposal& r, const EnvelopeMeta& m);
std::string encode_plan_variant(const TradePlanVariant& v, const EnvelopeMeta& m);
std::string encode_proposal(const TradeSignalProposal& s, const EnvelopeMeta& m);
// Drift snapshots, lifecycle transitions and outcome attribution are PERSISTED,
// not published: they are written to strategy_drift_snapshots, lifecycle_events
// and outcome_attribution. Encoders for them existed with no caller, while
// docs/NATS.md advertised the subjects as published -- a contract a consumer
// could subscribe to and wait on forever. The proto messages are kept for
// compatibility; the encoders are not.
std::string encode_market_outcome(const MarketOutcome& o, const EnvelopeMeta& m);
std::string encode_counterfactual(const CounterfactualOutcome& c, const EnvelopeMeta& m);

// --- parse path: AltML ---
struct ParsedForecast {
    int64_t canonical_pair_id = 0;
    int64_t feature_revision = 0;
    MarketForecastView view;
};
// Returns forecast/evaluation if the altml.v1 envelope carried one.
std::optional<ParsedForecast> parse_altml_forecast(std::string_view bytes);

// --- parse path: AltTech analytics component (tagged) ---
enum class AltTechKind {
    none, cross_venue_features, bitget_features, bitget_microstructure, global_regime,
    bitget_regime, projected_zone, consensus_zone, cross_venue_quality, universe_snapshot,
    universe_change,
    // A feature vector computed for an INFORMATION venue (Binance, OKX, Bybit).
    // These were discarded at the codec -- two `exchange() != EXCHANGE_BITGET`
    // gates turned them into `none` -- which is why DecisionContext declared a
    // reference_features summary that nothing could ever fill.
    reference_features,
    // Bitget's own 15m / 1h regime from AltTech's regime_module
    // (alttech.regime.venue.bitget.<sym>.combined.v1). Subscribed all along
    // and dropped here as `none`, so the only Bitget regime a decision ever saw
    // was the 1m venue_lite one.
    bitget_htf_regime
};
struct ParsedAltTech {
    AltTechKind kind = AltTechKind::none;
    // Which venue the payload is about. 1 Bitget, 2 Binance, 3 OKX, 4 Bybit,
    // matching AltTech's Exchange enum. Carried so a reference vector can never
    // be filed under the target venue by omission.
    int32_t venue = 0;
    int64_t canonical_pair_id = 0;
    int64_t revision = 0;
    int64_t bar_open_time_ms = 0;
    // Which tumbling window a microstructure reading covers ("1s".."1m").
    //
    // AltTech publishes the SAME 26-column set on five window lengths, and the
    // stream carries far more short windows than long ones (924k at 1s against
    // 17k at 1m). Without this the assembler could only order readings by time
    // and always kept the newest, which is a 1s window: flow columns arrived in
    // per-second units while the thresholds read them as per-minute, and the
    // book columns -- which a 1s window usually has no snapshot for -- were
    // present in 12% of contexts instead of all of them.
    std::string window_label;
    // Bar length of a per-timeframe vector, in minutes (AltTech's Timeframe enum
    // is valued in minutes). 0 when the payload is not timeframe-scoped.
    int32_t timeframe_minutes = 0;
    // Feature vectors decode named values via a feature-set mapping supplied by
    // config; here we surface the raw ordered values + the summary fields.
    std::vector<float> values;
    // The producer's identity for `values`. A positional decode is only sound
    // when the consumer's ordered name list is the one that produced this
    // vector, so the identity has to travel with the payload and be checked —
    // it is what distinguishes "feature 9 is atr_14" from a silent mislabel.
    std::string feature_set_name;
    int32_t feature_set_version = 0;
    std::string schema_hash;
    // Bit i clear => values[i] has no usable value (indicator not converged,
    // input missing). Never treat a cleared slot as the number that happens to
    // sit in the slot.
    std::string validity_bitmap;
    FeatureSummary features;        // populated when the mapping is known
    RegimeView regime;
    ProjectedZone zone;
    ContextQuality quality = ContextQuality::strict_4_of_4;
    uint32_t venue_mask = 0;
    uint32_t missing_venue_mask = 0;
    bool regime_divergence = false;
    std::string universe_status;
    // bitget_htf_regime: (bar length in minutes, regime) per timeframe.
    std::vector<std::pair<int32_t, RegimeView>> htf_regimes;
};
std::optional<ParsedAltTech> parse_alttech(std::string_view bytes);

// --- parse path: optional VPS2 execution feedback (altstrategy.v1 envelope) ---
std::optional<ExecutionFeedback> parse_execution_feedback(std::string_view bytes);

} // namespace altstrategy::engine_io
