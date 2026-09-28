#include "venue_sweep.hpp"

#include "reference_engine.hpp"
#include "reference_features.hpp"

#include "alttech/common/shutdown.hpp"
#include "alttech/config/config.hpp"
#include "alttech/logging/logger.hpp"
#include "alttech/messaging/codec.hpp"
#include "alttech/messaging/nats_client.hpp"
#include "alttech/messaging/topology.hpp"
#include "alttech/time/time.hpp"

#include "system.pb.h"

#include <chrono>
#include <string>
#include <vector>

using namespace alttech;

namespace alttech::venuelite {
namespace {

const char* kind_name(store::VenueOutput k) {
    switch (k) {
        case store::VenueOutput::feature: return "feature";
        case store::VenueOutput::regime: return "regime";
        case store::VenueOutput::microstructure: return "microstructure";
        case store::VenueOutput::structure: return "structure";
    }
    return "?";
}

::alttech::v1::Exchange to_proto_exchange(Venue v) {
    switch (v) {
        case Venue::bitget: return ::alttech::v1::EXCHANGE_BITGET;
        case Venue::binance_usdm: return ::alttech::v1::EXCHANGE_BINANCE;
        case Venue::okx: return ::alttech::v1::EXCHANGE_OKX;
        case Venue::bybit: return ::alttech::v1::EXCHANGE_BYBIT;
        case Venue::unknown: return ::alttech::v1::EXCHANGE_UNSPECIFIED;
    }
    return ::alttech::v1::EXCHANGE_UNSPECIFIED;
}

::alttech::v1::Envelope base_envelope(const store::PairVenue& pv, std::string_view event_type) {
    ::alttech::v1::Envelope env;
    env.set_schema_version(messaging::kSchemaVersion);
    env.set_event_type(std::string(event_type));
    env.set_exchange(to_proto_exchange(pv.venue));
    env.set_instrument_id(pv.canonical_pair_id);
    env.set_canonical_pair_id(pv.canonical_pair_id);
    env.set_exchange_symbol(pv.exchange_symbol);
    env.set_produced_at_ms(timeutil::now_ms());
    env.set_source_service("alttech-service");
    env.set_market_type(::alttech::v1::MARKET_TYPE_USDT_PERP);
    return env;
}

::alttech::v1::GlobalRegime to_proto_regime(VenueRegime r) {
    switch (r) {
        case VenueRegime::trend_up: return ::alttech::v1::GLOBAL_REGIME_TREND_UP;
        case VenueRegime::trend_down: return ::alttech::v1::GLOBAL_REGIME_TREND_DOWN;
        case VenueRegime::range: return ::alttech::v1::GLOBAL_REGIME_RANGE;
        case VenueRegime::breakout: return ::alttech::v1::GLOBAL_REGIME_BREAKOUT;
        case VenueRegime::volatility_expansion: return ::alttech::v1::GLOBAL_REGIME_VOLATILITY_EXPANSION;
        case VenueRegime::volatility_compression: return ::alttech::v1::GLOBAL_REGIME_VOLATILITY_COMPRESSION;
        case VenueRegime::chaotic: return ::alttech::v1::GLOBAL_REGIME_CHAOTIC;
        case VenueRegime::low_liquidity: return ::alttech::v1::GLOBAL_REGIME_LOW_LIQUIDITY;
        case VenueRegime::undefined: return ::alttech::v1::GLOBAL_REGIME_UNDEFINED;
    }
    return ::alttech::v1::GLOBAL_REGIME_UNSPECIFIED;
}

bool validate_venue_schema(persistence::PgPool& pool) {
    bool ok = false;
    auto res = pool.with_txn([&](pqxx::work& txn) {
        ok = txn.exec("SELECT to_regclass('alttech.venue_feature_vectors') IS NOT NULL "
                      "AND to_regclass('alttech.venue_regimes') IS NOT NULL "
                      "AND to_regclass('alttech.venue_microstructure_vectors') IS NOT NULL "
                      "AND to_regclass('alttech.venue_structure_zones') IS NOT NULL")[0][0]
                 .as<bool>();
    });
    return res && ok;
}

void publish_feature(messaging::NatsClient& nats, const store::PairVenue& pv, int64_t bar_ms,
                     int64_t revision, const RefResult& res) {
    auto env = base_envelope(pv, "feature.venue");
    auto* fv = env.mutable_venue_feature_vector();
    fv->set_scope(::alttech::v1::ANALYTICS_SCOPE_VENUE);
    fv->set_exchange(to_proto_exchange(pv.venue));
    fv->set_canonical_pair_id(pv.canonical_pair_id);
    fv->set_instrument_id(pv.canonical_pair_id);
    fv->set_timeframe(::alttech::v1::TF_1M);
    fv->set_bar_open_time_ms(bar_ms);
    fv->set_bar_revision(revision);
    fv->set_profile("reference_lite");
    fv->set_feature_set_name(std::string(kReferenceSetName));
    fv->set_feature_set_version(kReferenceSetVersion);
    fv->set_engine_version(std::string(kReferenceEngine));
    for (float v : res.values) fv->add_values(v);
    fv->set_validity_bitmap(std::string(reinterpret_cast<const char*>(res.validity.data()),
                                        res.validity.size()));
    fv->set_status(res.warming ? ::alttech::v1::ANALYTICS_VECTOR_STATUS_WARMING
                               : ::alttech::v1::ANALYTICS_VECTOR_STATUS_VALID);
    fv->set_computed_at_ms(timeutil::now_ms());
    // The venue_feature row is already committed; the deterministic message id
    // lets JetStream dedup a re-swept bar in the ALTTECH_ANALYTICS stream.
    const std::string msg_id = std::string("vf:").append(venue_code(pv.venue)).append(":")
                                   .append(pv.exchange_symbol).append(":")
                                   .append(std::to_string(bar_ms)).append(":r")
                                   .append(std::to_string(revision));
    (void)nats.js_publish(messaging::topology::venue_feature_subject(pv.venue, pv.exchange_symbol,
                                                                     Timeframe::m1),
                          messaging::serialize(env), msg_id, 2000);
}

void publish_regime(messaging::NatsClient& nats, const store::PairVenue& pv, int64_t bar_ms,
                    const RefResult& res) {
    auto env = base_envelope(pv, "regime.venue");
    auto* r = env.mutable_venue_market_regime();
    r->set_scope(::alttech::v1::ANALYTICS_SCOPE_VENUE);
    r->set_exchange(to_proto_exchange(pv.venue));
    r->set_canonical_pair_id(pv.canonical_pair_id);
    r->set_instrument_id(pv.canonical_pair_id);
    r->set_timeframe(::alttech::v1::TF_1M);
    r->set_bar_open_time_ms(bar_ms);
    r->set_regime(to_proto_regime(res.regime));
    r->set_confidence(res.regime_confidence);
    r->set_trend_strength(res.trend_strength);
    r->set_volatility_regime(res.volatility_regime);
    r->set_liquidity_quality(res.liquidity_quality);
    r->set_algorithm_version("reference-regime-1");
    const std::string msg_id = std::string("vr:").append(venue_code(pv.venue)).append(":")
                                   .append(pv.exchange_symbol).append(":")
                                   .append(std::to_string(bar_ms));
    (void)nats.js_publish(messaging::topology::venue_regime_subject(pv.venue, pv.exchange_symbol,
                                                                    Timeframe::m1),
                          messaging::serialize(env), msg_id, 2000);
}

void publish_zone(messaging::NatsClient& nats, const store::PairVenue& pv, const SwingZone& z) {
    auto env = base_envelope(pv, "structure.venue");
    auto* zone = env.mutable_venue_structure_zone();
    zone->set_scope(::alttech::v1::ANALYTICS_SCOPE_VENUE);
    zone->set_exchange(to_proto_exchange(pv.venue));
    zone->set_canonical_pair_id(pv.canonical_pair_id);
    zone->set_instrument_id(pv.canonical_pair_id);
    zone->set_timeframe(::alttech::v1::TF_1M);
    zone->set_kind(z.resistance ? "resistance" : "support");
    zone->set_lower_price(z.lower);
    zone->set_center_price(z.center);
    zone->set_upper_price(z.upper);
    zone->set_lifecycle(::alttech::v1::ZONE_LIFECYCLE_ACTIVE);
    zone->set_algorithm_version("venue-swing-1");
    zone->set_updated_at_ms(timeutil::now_ms());
    // Keyed by geometry so an unchanged zone dedups but a moved zone re-emits.
    const std::string msg_id = std::string("vz:").append(venue_code(pv.venue)).append(":")
                                   .append(pv.exchange_symbol).append(":")
                                   .append(z.resistance ? "resistance" : "support").append(":")
                                   .append(std::to_string(static_cast<int64_t>(z.center * 1e6)));
    (void)nats.js_publish(messaging::topology::venue_structure_subject(pv.venue, pv.exchange_symbol,
                                                                       Timeframe::m1),
                          messaging::serialize(env), msg_id, 2000);
}

} // namespace

void run_venue_sweep(persistence::PgPool& pool, messaging::NatsClient& nats,
                     store::VenueOutput kind, const YAML::Node& module_config) {
    auto& shutdown = ShutdownToken::instance();
    const char* name = kind_name(kind);

    if (!validate_venue_schema(pool)) {
        log::critical("venue_sweep_schema_missing",
                      {{"kind", name},
                       {"message", "alttech venue tables missing; run alttech-db-migrate"}});
        shutdown.request_stop(); // fail-closed on schema/contract integrity
        return;
    }

    int64_t feature_set_id = 0;
    if (kind == store::VenueOutput::feature) {
        auto id = store::ensure_reference_feature_set(pool);
        if (!id) { log::error("venue_sweep_set_error", {{"message", id.error().message}}); return; }
        feature_set_id = *id;
    } else if (kind == store::VenueOutput::microstructure) {
        auto id = store::ensure_reference_micro_set(pool);
        if (!id) { log::error("venue_sweep_set_error", {{"message", id.error().message}}); return; }
        feature_set_id = *id;
    }

    const int64_t cycle = std::clamp<int64_t>(
        config::opt_int(module_config, "cycle_interval_sec", 30), 5, 3600);
    const int64_t watermark = std::clamp<int64_t>(
        config::opt_int(module_config, "venue_bar_watermark_ms", 4000), 0, 60000);
    const int64_t lookback = std::clamp<int64_t>(
        config::opt_int(module_config, "venue_lookback_min", 180), 5, 10080);
    const int window_len = static_cast<int>(std::clamp<int64_t>(
        config::opt_int(module_config, "venue_window_len", 220), 20, 512));

    auto pvs = store::load_pair_venues(pool, store::scope_for(kind));
    if (!pvs) { log::error("venue_sweep_universe_error", {{"message", pvs.error().message}}); return; }
    log::info("venue_sweep_ready", {{"kind", name}, {"pair_venues", static_cast<int64_t>(pvs->size())}});

    int64_t last_reload = timeutil::now_ms();
    while (!shutdown.stop_requested()) {
        int64_t produced = 0;
        for (const auto& pv : *pvs) {
            if (shutdown.stop_requested()) break;
            auto bar = store::next_venue_bar(pool, pv, kind, watermark, static_cast<int>(lookback), feature_set_id);
            if (!bar) continue;      // transient DB error -> skip, stay alive
            if (!*bar) continue;

            auto window = store::load_candle_window(pool, pv, **bar, window_len);
            if (!window || window->empty()) continue;
            const double close = window->back().close;
            const int64_t revision = window->back().revision;

            switch (kind) {
                case store::VenueOutput::feature: {
                    auto ctx = store::load_context(pool, pv, **bar);
                    if (!ctx) continue;
                    // The primary venue already has the full indicator ladder
                    // from the TA-Lib engine; here it contributes only what
                    // that engine cannot see.
                    const auto res = is_primary(pv.venue) ? compute_bbo_only(*ctx)
                                                                 : compute_reference(*window, *ctx);
                    if (store::write_venue_feature(pool, pv, **bar, revision, feature_set_id, res)) {
                        publish_feature(nats, pv, **bar, revision, res);
                        ++produced;
                    }
                    break;
                }
                case store::VenueOutput::regime: {
                    auto ctx = store::load_context(pool, pv, **bar);
                    if (!ctx) continue;
                    const auto res = compute_reference(*window, *ctx);
                    if (store::write_venue_regime(pool, pv, **bar, res)) {
                        publish_regime(nats, pv, **bar, res);
                        ++produced;
                    }
                    break;
                }
                case store::VenueOutput::microstructure: {
                    auto ctx = store::load_context(pool, pv, **bar);
                    if (!ctx) continue;
                    if (store::write_venue_micro(pool, pv, **bar, feature_set_id, *ctx, close)) {
                        ++produced;
                    }
                    break;
                }
                case store::VenueOutput::structure: {
                    const auto z = detect_swing_zone(*window);
                    if (z && store::write_venue_zone(pool, pv, *z)) {
                        publish_zone(nats, pv, *z);
                        ++produced;
                    }
                    break;
                }
            }
        }
        if (produced > 0) log::info("venue_sweep_produced", {{"kind", name}, {"count", produced}});

        const int64_t now = timeutil::now_ms();
        if (now - last_reload > 300'000) {
            if (auto u = store::load_pair_venues(pool, store::scope_for(kind))) {
                *pvs = std::move(*u);
                last_reload = now;
            }
        }
        if (shutdown.wait_for(std::chrono::seconds(cycle))) break;
    }
    log::info("venue_sweep_stopped", {{"kind", name}});
}

} // namespace alttech::venuelite
