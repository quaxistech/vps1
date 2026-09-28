#include "altstrategy/engine_io/context_replay.hpp"

#include "altstrategy/engine_io/context_json.hpp"

#include <cmath>

namespace altstrategy::engine_io {
namespace {

using nlohmann::json;

void load_values(const json& obj, engine::FeatureSummary& out) {
    if (!obj.is_object()) return;
    for (const auto& [k, v] : obj.items()) {
        if (!v.is_number()) continue;
        const double d = v.get<double>();
        if (!std::isfinite(d)) continue;   // the encoder omits these; be strict on the way back too
        out.values[k] = d;
    }
    out.valid = !out.values.empty();
}

engine::RegimeView load_regime(const json& obj) {
    engine::RegimeView r;
    if (!obj.is_object()) return r;
    if (obj.contains("regime") && obj["regime"].is_string()) r.regime = obj["regime"].get<std::string>();
    const auto num = [&](const char* k, double& dst) {
        if (obj.contains(k) && obj[k].is_number()) dst = obj[k].get<double>();
    };
    num("confidence", r.confidence);
    num("trend_strength", r.trend_strength);
    num("volatility_regime", r.volatility_regime);
    num("liquidity_quality", r.liquidity_quality);
    return r;
}

} // namespace

std::optional<engine::DecisionContext> context_from_snapshot(std::string_view snapshot_json,
                                                             const SnapshotScalars& s) {
    json j = json::parse(snapshot_json, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return std::nullopt;

    engine::DecisionContext c;
    load_values(j.value("bitget_features", json::object()), c.bitget_features);
    if (c.bitget_features.values.empty()) return std::nullopt;
    load_values(j.value("microstructure", json::object()), c.microstructure);
    load_values(j.value("cross_venue_features", json::object()), c.cross_venue_features);

    if (const auto& refs = j["reference_features"]; refs.is_object()) {
        for (const auto& [venue, values] : refs.items()) {
            engine::FeatureSummary fs;
            load_values(values, fs);
            if (!fs.values.empty()) c.reference_features[std::stoi(venue)] = std::move(fs);
        }
    }
    if (const auto& grids = j["context_features"]; grids.is_object()) {
        for (const auto& [tf, blob] : grids.items()) {
            if (!blob.is_object()) continue;
            engine::FeatureSummary fs;
            load_values(blob.value("values", json::object()), fs);
            if (blob.contains("bar_open_time_ms") && blob["bar_open_time_ms"].is_number_integer())
                fs.bar_open_time_ms = blob["bar_open_time_ms"].get<int64_t>();
            if (!fs.values.empty()) c.context_features[std::stoi(tf)] = std::move(fs);
        }
    }
    c.bitget_regime = load_regime(j.value("bitget_regime_view", json::object()));
    c.global_regime = load_regime(j.value("global_regime_view", json::object()));

    c.canonical_pair_id = s.canonical_pair_id;
    c.bitget_instrument_id = s.bitget_instrument_id;
    c.context_revision = s.context_revision;
    c.event_time_ms = s.event_time_ms;
    c.available_at_ms = s.available_at_ms;
    c.bitget_features.bar_open_time_ms = s.event_time_ms;
    c.data_quality = engine::quality_from_string(s.data_quality);
    c.global_universe_status = s.universe_status;
    c.feature_manifest_hash = s.feature_manifest_hash;
    c.venue_mask = s.venue_mask;
    c.missing_venue_mask = s.missing_venue_mask;
    c.regime_divergence = s.regime_divergence;
    c.fees_bps = s.fees_bps;
    c.funding_rate = s.funding_rate;
    c.mark_price = s.mark_price;
    c.spread_bps = s.spread_bps;
    c.top_depth_usdt = s.top_depth_usdt;
    c.liquidity_rank = s.liquidity_rank;
    c.expected_slippage_bps = s.expected_slippage_bps;
    c.spread_observed = s.spread_observed;
    c.depth_observed = s.depth_observed;
    c.symbol = s.symbol;
    // market_forecast stays absent: the snapshot never carried the prediction,
    // and reading today's prediction into a two-week-old context would be
    // lookahead of the worst kind.
    return c;
}

} // namespace altstrategy::engine_io
