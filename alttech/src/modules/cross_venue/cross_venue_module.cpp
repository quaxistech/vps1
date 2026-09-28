// Cross-venue module.
//
// It aligns the four venues' closed 1m bars on event time and then computes,
// for one canonical pair at a time: a quality-weighted price consensus and the
// per-venue divergences from it, momentum and order-flow agreement, funding and
// open-interest normalized across venues, a liquidity ranking, a bounded
// lead-lag estimate, the global market regime, and the structure zones the
// venues agree on together with their projection onto the primary venue.
//
// Cadence is a bounded PostgreSQL-driven sweep rather than per-event work. A
// transient reference-venue outage degrades the window quality (DEGRADED_*) but
// never stops the module; only a schema or contract integrity breach is fatal.

#include "cross_venue_features.hpp"
#include "cross_venue_store.hpp"

#include "alignment.hpp"
#include "consensus.hpp"
#include "global_view.hpp"
#include "lead_lag.hpp"
#include "normalization.hpp"
#include "regime_readiness.hpp"

#include <atomic>
#include <span>

#include "alttech/application/module_supervisor.hpp"
#include "alttech/common/shutdown.hpp"
#include "alttech/config/config.hpp"
#include "alttech/logging/logger.hpp"
#include "alttech/messaging/codec.hpp"
#include "alttech/messaging/nats_client.hpp"
#include "alttech/messaging/topology.hpp"
#include "alttech/modules/cross_venue.hpp"
#include "alttech/persistence/pg.hpp"
#include "alttech/time/time.hpp"

#include "system.pb.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

using namespace alttech;
using namespace alttech::crossvenue;

namespace {

constexpr std::string_view kModuleName = "cross_venue";

struct Params {
    bool enabled = true;
    int64_t cycle_interval_sec = 30;
    int64_t bar_watermark_ms = 4000;
    int64_t lookback_min = 180;
    ConsensusMethod method = ConsensusMethod::quality_weighted_median;
    WeightParams weights;
    QualityParams quality;
    AlignmentParams alignment;
    LeadLagParams lead_lag;
    int64_t return_series_len = 30;
    // A consensus zone no venue confirms for this long is expired.
    int64_t zone_stale_after_sec = 1800;
    int min_consensus_venues = 2;
};

ConsensusMethod method_from_string(const std::string& s) {
    if (s == "quality_weighted_mean") return ConsensusMethod::quality_weighted_mean;
    if (s == "trimmed_weighted_mean") return ConsensusMethod::trimmed_weighted_mean;
    return ConsensusMethod::quality_weighted_median;
}

Params parse_params(const YAML::Node& cfg) {
    Params p;
    p.enabled = config::opt_bool(cfg, "enabled", true);
    p.cycle_interval_sec = std::clamp<int64_t>(config::opt_int(cfg, "cycle_interval_sec", 30), 1, 3600);
    p.lookback_min = std::clamp<int64_t>(config::opt_int(cfg, "lookback_min", 180), 5, 10080);
    p.return_series_len = std::clamp<int64_t>(config::opt_int(cfg, "lead_lag_window", 30), 5, 240);
    const auto alignment = cfg["alignment"];
    p.bar_watermark_ms =
        std::clamp<int64_t>(config::opt_int(alignment, "bar_watermark_timeout_ms", 4000), 0, 60000);
    p.quality.max_cross_venue_skew_ms =
        static_cast<double>(config::opt_int(alignment, "max_cross_venue_skew_ms", 2500));
    p.quality.max_venue_data_age_ms =
        static_cast<double>(config::opt_int(alignment, "max_venue_data_age_ms", 180000));
    p.alignment.bar_watermark_timeout_ms = static_cast<double>(p.bar_watermark_ms);
    p.alignment.max_cross_venue_skew_ms = p.quality.max_cross_venue_skew_ms;
    p.lead_lag.max_lag = static_cast<int>(std::clamp<int64_t>(config::opt_int(cfg, "lead_lag_max_lag", 5), 1, 30));
    p.lead_lag.sample_interval_ms = 60000.0; // 1m grid (see load_return_series)
    const auto weighting = cfg["weighting"];
    p.method = method_from_string(config::opt_str(weighting, "method", "quality_weighted_median"));
    p.zone_stale_after_sec =
        std::clamp<int64_t>(config::opt_int(cfg, "zone_stale_after_sec", 1800), 60, 86400);
    p.min_consensus_venues = static_cast<int>(
        std::clamp<int64_t>(config::opt_int(cfg, "min_consensus_venues", 2), 2, 4));
    return p;
}

// --- validity bitmap helpers ---
void set_bit(std::vector<uint8_t>& bm, std::size_t i) { bm[i / 8] |= static_cast<uint8_t>(1U << (i % 8)); }

// Sign helper: -1, 0, +1.
int sgn(double x) { return (x > 0.0) - (x < 0.0); }

// Build the full cross-venue record from the aligned samples + return series.
store::CrossVenueRecord compute_record(int64_t canonical_pair_id, int64_t bar_open_ms,
                                       int64_t feature_set_id, std::span<const VenueSample> samples,
                                       std::span<const VenueSeries> series, const Params& p) {
    store::CrossVenueRecord rec;
    rec.canonical_pair_id = canonical_pair_id;
    rec.bar_open_ms = bar_open_ms;
    rec.canonical_close_ms = bar_open_ms + 60000;
    rec.feature_set_id = feature_set_id;
    rec.computed_at_ms = timeutil::now_ms();
    rec.values.assign(XV_FEATURE_COUNT, 0.0F);
    rec.validity.assign((XV_FEATURE_COUNT + 7) / 8, 0);

    // Alignment (elapsed since close is large in the sweep -> watermark elapsed).
    const double elapsed = static_cast<double>(rec.computed_at_ms - rec.canonical_close_ms);
    const auto align = evaluate_bar_window(samples, p.alignment, elapsed, false);
    rec.alignment = align.status;
    rec.max_skew_ms = align.max_skew_ms;

    // Quality + masks.
    const auto q = classify_quality(samples, p.quality, align.max_skew_ms);
    rec.quality = q.status;
    rec.venue_mask = q.venue_mask;
    rec.missing_venue_mask = q.missing_venue_mask;
    rec.stale_venue_mask = q.stale_venue_mask;
    rec.desynced_venue_mask = q.desynced_venue_mask;
    rec.confidence = q.confidence;
    rec.quality_reasons = q.reasons;

    const auto set_val = [&](std::size_t idx, double v) {
        rec.values[idx] = static_cast<float>(v);
        set_bit(rec.validity, idx);
    };

    // --- price consensus + deviations ---
    const auto mid = consensus(samples, p.weights, p.method,
                               [](const VenueSample& s) { return s.mid_price; });
    rec.contributions = mid.contributions;
    if (mid.available) {
        rec.consensus_mid = mid.value;
        set_val(XV_CONSENSUS_MID_PRICE, mid.value);
        double max_dev = 0.0;
        for (const auto& s : samples) {
            if (!s.mid_price) continue;
            const double dev = deviation_bps(*s.mid_price, mid.value);
            max_dev = std::max(max_dev, std::abs(dev));
            switch (s.venue) {
                case Venue::bitget:
                    rec.bitget_mid_dev_bps = dev; set_val(XV_BITGET_MID_DEVIATION_BPS, dev); break;
                case Venue::binance_usdm: set_val(XV_BINANCE_MID_DEVIATION_BPS, dev); break;
                case Venue::bybit: set_val(XV_BYBIT_MID_DEVIATION_BPS, dev); break;
                case Venue::okx: set_val(XV_OKX_MID_DEVIATION_BPS, dev); break;
                default: break;
            }
        }
        set_val(XV_MAX_VENUE_PRICE_DEVIATION_BPS, max_dev);
        std::vector<Contribution> incl;
        for (const auto& c : mid.contributions) if (c.included) incl.push_back(c);
        const double disp = dispersion_bps(incl, mid.value);
        rec.venue_price_dispersion = disp;
        set_val(XV_VENUE_PRICE_DISPERSION_BPS, disp);
    }
    const auto mark = consensus(samples, p.weights, p.method,
                                [](const VenueSample& s) { return s.mark_price; });
    if (mark.available) { rec.consensus_mark = mark.value; set_val(XV_CONSENSUS_MARK_PRICE, mark.value); }
    const auto index = consensus(samples, p.weights, p.method,
                                 [](const VenueSample& s) { return s.index_price; });
    if (index.available) { rec.consensus_index = index.value; set_val(XV_CONSENSUS_INDEX_PRICE, index.value); }

    // --- momentum + trend agreement ---
    const auto ret = consensus(samples, p.weights, p.method,
                               [](const VenueSample& s) { return s.log_return; });
    if (ret.available) {
        set_val(XV_CONSENSUS_MOMENTUM_SCORE, ret.value);
        int agree = 0, total = 0;
        std::vector<double> rets;
        for (const auto& s : samples) {
            if (!s.log_return) continue;
            ++total;
            rets.push_back(*s.log_return);
            if (sgn(*s.log_return) == sgn(ret.value)) ++agree;
            if (s.venue == Venue::bitget) set_val(XV_BITGET_MOMENTUM_DIVERGENCE, *s.log_return - ret.value);
        }
        if (total > 0) set_val(XV_VENUE_MOMENTUM_AGREEMENT, static_cast<double>(agree) / total);
        if (rets.size() >= 2) set_val(XV_VENUE_RETURN_DISPERSION, stddev(rets));
    }

    // --- trade flow; volumes are already USDT-normalized ---
    double signed_sum = 0.0;
    int flow_total = 0, flow_agree = 0;
    std::vector<double> flows;
    bool any_flow = false;
    for (const auto& s : samples) {
        if (!s.signed_volume_usdt) continue;
        any_flow = true;
        signed_sum += *s.signed_volume_usdt;
        flows.push_back(*s.signed_volume_usdt);
    }
    if (any_flow) {
        set_val(XV_CROSS_VENUE_SIGNED_VOLUME, signed_sum);
        for (double f : flows) { ++flow_total; if (sgn(f) == sgn(signed_sum)) ++flow_agree; }
        if (flow_total > 0) set_val(XV_ORDERFLOW_AGREEMENT, static_cast<double>(flow_agree) / flow_total);
        if (flows.size() >= 2) set_val(XV_ORDERFLOW_DISPERSION, stddev(flows));
    }

    // --- funding ---
    {
        std::vector<VenueSample> fund_samples;
        double bitget_8h = 0.0; bool bitget_has = false;
        std::vector<double> f8s;
        for (const auto& s : samples) {
            if (!s.funding_rate || !s.funding_interval_hours) continue;
            const auto nf = normalize_funding(*s.funding_rate, *s.funding_interval_hours);
            if (!nf) continue;
            VenueSample fs = s;
            fs.mid_price = nf->rate_8h_equivalent; // reuse consensus over 8h-equiv
            fund_samples.push_back(fs);
            f8s.push_back(nf->rate_8h_equivalent);
            if (s.venue == Venue::bitget) { bitget_8h = nf->rate_8h_equivalent; bitget_has = true; }
        }
        if (!fund_samples.empty()) {
            const auto cf = consensus(fund_samples, p.weights, p.method,
                                      [](const VenueSample& s) { return s.mid_price; });
            if (cf.available) {
                set_val(XV_CONSENSUS_FUNDING_8H, cf.value);
                if (f8s.size() >= 2) set_val(XV_FUNDING_DISPERSION, stddev(f8s));
                if (bitget_has) set_val(XV_BITGET_FUNDING_DEVIATION, bitget_8h - cf.value);
            }
        }
    }

    // --- open interest ---
    {
        std::vector<double> ois;
        double bitget_oi = 0.0; bool bitget_has = false;
        std::vector<VenueSample> oi_samples;
        for (const auto& s : samples) {
            if (!s.oi_change_ratio) continue;
            ois.push_back(*s.oi_change_ratio);
            VenueSample os = s; os.mid_price = *s.oi_change_ratio;
            oi_samples.push_back(os);
            if (s.venue == Venue::bitget) { bitget_oi = *s.oi_change_ratio; bitget_has = true; }
        }
        if (!oi_samples.empty()) {
            const auto co = consensus(oi_samples, p.weights, p.method,
                                      [](const VenueSample& s) { return s.mid_price; });
            if (co.available) {
                set_val(XV_CONSENSUS_OI_CHANGE, co.value);
                int total = 0, agree = 0;
                for (double v : ois) { ++total; if (sgn(v) == sgn(co.value)) ++agree; }
                if (total > 0) set_val(XV_OI_DIRECTION_AGREEMENT, static_cast<double>(agree) / total);
                if (bitget_has) set_val(XV_BITGET_OI_DEVIATION, bitget_oi - co.value);
            }
        }
    }

    // --- technical agreement across venues (v2) -----------------------------
    // Each venue's indicators are computed independently from its own candles,
    // so agreement between them is genuine confirmation rather than the same
    // number counted four times. Dispersion is reported next to every consensus
    // so a consumer can tell "all four agree" from "they average out".
    {
        std::vector<double> rsis;
        std::vector<VenueSample> rsi_samples;
        double bitget_rsi = 0.0; bool bitget_rsi_has = false;
        for (const auto& s : samples) {
            if (!s.rsi_14) continue;
            rsis.push_back(*s.rsi_14);
            VenueSample rs = s; rs.mid_price = *s.rsi_14;   // reuse the estimator
            rsi_samples.push_back(rs);
            if (s.venue == Venue::bitget) { bitget_rsi = *s.rsi_14; bitget_rsi_has = true; }
        }
        if (!rsi_samples.empty()) {
            const auto cr = consensus(rsi_samples, p.weights, p.method,
                                      [](const VenueSample& s) { return s.mid_price; });
            if (cr.available) {
                set_val(XV_CONSENSUS_RSI_14, cr.value);
                if (rsis.size() >= 2) set_val(XV_RSI_DISPERSION, stddev(rsis));
                if (bitget_rsi_has) set_val(XV_BITGET_RSI_DEVIATION, bitget_rsi - cr.value);
            }
        }

        std::vector<double> vols;
        std::vector<VenueSample> vol_samples;
        for (const auto& s : samples) {
            if (!s.realized_vol_20 || !(*s.realized_vol_20 > 0.0)) continue;
            vols.push_back(*s.realized_vol_20);
            VenueSample vs = s; vs.mid_price = *s.realized_vol_20;
            vol_samples.push_back(vs);
        }
        if (!vol_samples.empty()) {
            const auto cv = consensus(vol_samples, p.weights, p.method,
                                      [](const VenueSample& s) { return s.mid_price; });
            if (cv.available && cv.value > 0.0) {
                set_val(XV_CONSENSUS_REALIZED_VOL, cv.value);
                // Agreement as a coefficient of variation folded into [0,1]:
                // identical readings give 1, a spread as wide as the level itself
                // gives 0. Scale-free, so it is comparable across instruments.
                if (vols.size() >= 2) {
                    const double cvar = stddev(vols) / cv.value;
                    set_val(XV_VOLATILITY_AGREEMENT, std::clamp(1.0 - cvar, 0.0, 1.0));
                }
            }
        }

        double trend_sum = 0.0;
        int trend_n = 0;
        double bitget_trend = 0.0; bool bitget_trend_has = false;
        for (const auto& s : samples) {
            if (!s.trend_alignment) continue;
            trend_sum += *s.trend_alignment;
            ++trend_n;
            if (s.venue == Venue::bitget) { bitget_trend = *s.trend_alignment; bitget_trend_has = true; }
        }
        if (trend_n > 0) {
            const double mean_trend = trend_sum / trend_n;
            set_val(XV_TREND_ALIGNMENT_AGREEMENT, mean_trend);
            if (bitget_trend_has) set_val(XV_BITGET_TREND_DIVERGENCE, bitget_trend - mean_trend);
        }
    }

    // --- liquidity ---
    {
        std::vector<double> spreads;
        double bitget_spread = 0.0; bool bitget_has = false;
        for (const auto& s : samples) {
            if (!s.relative_spread) continue;
            spreads.push_back(*s.relative_spread);
            if (s.venue == Venue::bitget) { bitget_spread = *s.relative_spread; bitget_has = true; }
        }
        if (spreads.size() >= 2) set_val(XV_CROSS_VENUE_SPREAD_DISPERSION, stddev(spreads));
        if (bitget_has && !spreads.empty()) {
            int rank = 1;
            for (double sp : spreads) if (sp < bitget_spread) ++rank; // 1 = tightest
            set_val(XV_BITGET_LIQUIDITY_RANK, static_cast<double>(rank));
        }
    }

    // --- lead-lag ---
    if (!series.empty()) {
        const auto ll = compute_lead_lag(series, p.lead_lag);
        rec.dominant_leading_venue = ll.dominant_leading_venue;
        rec.lead_confidence = ll.lead_confidence;
        set_val(XV_BINANCE_LEAD_SCORE, ll.lead_score[static_cast<std::size_t>(venue_bit(Venue::binance_usdm))]);
        set_val(XV_BYBIT_LEAD_SCORE, ll.lead_score[static_cast<std::size_t>(venue_bit(Venue::bybit))]);
        set_val(XV_OKX_LEAD_SCORE, ll.lead_score[static_cast<std::size_t>(venue_bit(Venue::okx))]);
        set_val(XV_BITGET_LEAD_SCORE, ll.lead_score[static_cast<std::size_t>(venue_bit(Venue::bitget))]);
        const double lag = (ll.bitget_lag_to_binance_ms + ll.bitget_lag_to_bybit_ms +
                            ll.bitget_lag_to_okx_ms) / 3.0;
        set_val(XV_BITGET_LAG_TO_CONSENSUS_MS, lag);
    }

    set_val(XV_CONFIDENCE, rec.confidence);
    return rec;
}

// Build a cross-venue Envelope with the four-venue metadata set.
::alttech::v1::Envelope base_envelope(int64_t canonical_pair_id, std::string_view symbol,
                                      std::string_view event_type) {
    ::alttech::v1::Envelope env;
    env.set_schema_version(messaging::kSchemaVersion);
    env.set_event_type(std::string(event_type));
    env.set_exchange(::alttech::v1::EXCHANGE_UNSPECIFIED);
    env.set_instrument_id(canonical_pair_id);
    env.set_canonical_pair_id(canonical_pair_id);
    env.set_exchange_symbol(std::string(symbol));
    env.set_produced_at_ms(timeutil::now_ms());
    env.set_source_service("alttech-service");
    env.set_market_type(::alttech::v1::MARKET_TYPE_USDT_PERP);
    return env;
}

::alttech::v1::CrossVenueQualityStatus to_proto_quality(CrossVenueQuality q) {
    switch (q) {
        case CrossVenueQuality::strict_valid:
            return ::alttech::v1::CROSS_VENUE_QUALITY_STRICT_VALID;
        case CrossVenueQuality::degraded_3_of_4:
            return ::alttech::v1::CROSS_VENUE_QUALITY_DEGRADED_3_OF_4;
        case CrossVenueQuality::degraded_2_of_4:
            return ::alttech::v1::CROSS_VENUE_QUALITY_DEGRADED_2_OF_4;
        case CrossVenueQuality::invalid:
            return ::alttech::v1::CROSS_VENUE_QUALITY_INVALID;
    }
    return ::alttech::v1::CROSS_VENUE_QUALITY_INVALID;
}

::alttech::v1::GlobalRegime to_proto_global_regime(std::string_view regime) {
    if (regime == "TREND_UP") return ::alttech::v1::GLOBAL_REGIME_TREND_UP;
    if (regime == "TREND_DOWN") return ::alttech::v1::GLOBAL_REGIME_TREND_DOWN;
    if (regime == "RANGE") return ::alttech::v1::GLOBAL_REGIME_RANGE;
    if (regime == "BREAKOUT") return ::alttech::v1::GLOBAL_REGIME_BREAKOUT;
    if (regime == "VOLATILITY_EXPANSION") return ::alttech::v1::GLOBAL_REGIME_VOLATILITY_EXPANSION;
    if (regime == "VOLATILITY_COMPRESSION") {
        return ::alttech::v1::GLOBAL_REGIME_VOLATILITY_COMPRESSION;
    }
    if (regime == "CHAOTIC") return ::alttech::v1::GLOBAL_REGIME_CHAOTIC;
    if (regime == "LOW_LIQUIDITY") return ::alttech::v1::GLOBAL_REGIME_LOW_LIQUIDITY;
    return ::alttech::v1::GLOBAL_REGIME_UNDEFINED;
}

void publish_global_regime(messaging::NatsClient& nats, std::string_view symbol,
                           const store::CrossVenueRecord& rec, const GlobalRegimeResult& regime) {
    if (!regime.available) return;
    auto env = base_envelope(rec.canonical_pair_id, symbol, "regime.cross_venue");
    auto* g = env.mutable_global_market_regime();
    g->set_scope(::alttech::v1::ANALYTICS_SCOPE_CROSS_VENUE);
    g->set_canonical_pair_id(rec.canonical_pair_id);
    g->set_timeframe(::alttech::v1::TF_1M);
    g->set_bar_open_time_ms(rec.bar_open_ms);
    g->set_primary_regime(to_proto_global_regime(regime.primary_regime));
    g->set_regime_confidence(regime.regime_confidence);
    g->set_venue_agreement(regime.venue_agreement);
    g->set_venue_dispersion(regime.venue_dispersion);
    g->set_supporting_venue_mask(regime.supporting_venue_mask);
    g->set_bitget_regime_divergence(regime.bitget_divergence);
    g->set_trend_consensus(regime.trend_consensus);
    g->set_volatility_consensus(regime.volatility_consensus);
    g->set_liquidity_consensus(regime.liquidity_consensus);
    g->set_quality_status(to_proto_quality(rec.quality));
    g->set_algorithm_version("global-regime-1");
    const std::string msg_id = "xvr:" + std::to_string(rec.canonical_pair_id) + ":" +
                               std::to_string(rec.bar_open_ms);
    (void)nats.js_publish(messaging::topology::cross_venue_regime_subject(symbol, Timeframe::m1),
                          messaging::serialize(env), msg_id, 2000);
}

void publish_consensus_zones(messaging::NatsClient& nats, std::string_view symbol,
                             const store::CrossVenueRecord& rec,
                             std::span<const ConsensusZone> zones, double primary_mid) {
    for (const auto& zone : zones) {
        auto env = base_envelope(rec.canonical_pair_id, symbol, "structure.cross_venue");
        auto* z = env.mutable_consensus_structure_zone();
        z->set_scope(::alttech::v1::ANALYTICS_SCOPE_CROSS_VENUE);
        z->set_canonical_pair_id(rec.canonical_pair_id);
        z->set_timeframe(::alttech::v1::TF_1M);
        z->set_normalized_center(zone.normalized_center);
        z->set_normalized_width(zone.normalized_width);
        z->set_supporting_venue_mask(zone.supporting_venue_mask);
        z->set_supporting_venue_count(zone.supporting_venue_count);
        z->set_venue_confluence_score(zone.venue_confluence_score);
        z->set_strength(zone.strength);
        z->set_confidence(zone.confidence);
        z->set_lifecycle(zone.supporting_venue_count >= 3 ? ::alttech::v1::ZONE_LIFECYCLE_ACTIVE
                                                          : ::alttech::v1::ZONE_LIFECYCLE_CANDIDATE);
        z->set_quality_status(to_proto_quality(rec.quality));
        z->set_algorithm_version("consensus-zone-1");
        z->set_updated_at_ms(rec.computed_at_ms);
        // Keyed by the level itself, so an unchanged zone dedups and a moved
        // one re-emits.
        const std::string msg_id =
            "xvz:" + std::to_string(rec.canonical_pair_id) + ":" + zone.kind + ":" +
            std::to_string(static_cast<int64_t>(zone.normalized_center * 1e6));
        (void)nats.js_publish(
            messaging::topology::cross_venue_structure_subject(symbol, Timeframe::m1),
            messaging::serialize(env), msg_id, 2000);

        if (primary_mid > 0.0) {
            const auto projected = project_to_primary(zone, primary_mid,
                                                      rec.consensus_mid.value_or(0.0),
                                                      rec.confidence);
            auto penv = base_envelope(rec.canonical_pair_id, symbol, "structure.bitget_projected");
            auto* p = penv.mutable_bitget_projected_zone();
            p->set_scope(::alttech::v1::ANALYTICS_SCOPE_BITGET_PROJECTED);
            p->set_canonical_pair_id(rec.canonical_pair_id);
            p->set_timeframe(::alttech::v1::TF_1M);
            p->set_bitget_basis_adjustment(projected.basis_adjustment);
            p->set_projected_lower(projected.lower);
            p->set_projected_center(projected.center);
            p->set_projected_upper(projected.upper);
            p->set_projection_quality(projected.quality);
            p->set_projection_time_ms(rec.computed_at_ms);
            p->set_algorithm_version("consensus-zone-1");
            (void)nats.js_publish(
                messaging::topology::bitget_projected_structure_subject(symbol, Timeframe::m1),
                messaging::serialize(penv), "xvp:" + msg_id, 2000);
        }
    }
}

void publish_outputs(messaging::NatsClient& nats, std::string_view symbol,
                     const store::CrossVenueRecord& rec) {
    // Cross-venue feature vector.
    {
        auto env = base_envelope(rec.canonical_pair_id, symbol, "feature.cross_venue");
        auto* fv = env.mutable_cross_venue_feature_vector();
        fv->set_scope(::alttech::v1::ANALYTICS_SCOPE_CROSS_VENUE);
        fv->set_canonical_pair_id(rec.canonical_pair_id);
        fv->set_timeframe(::alttech::v1::TF_1M);
        fv->set_bar_open_time_ms(rec.bar_open_ms);
        fv->set_feature_set_name(std::string(kCrossVenueSetName));
        fv->set_feature_set_version(kCrossVenueSetVersion);
        fv->set_method_version(std::string(kCrossVenueMethodVersion));
        for (float v : rec.values) fv->add_values(v);
        fv->set_validity_bitmap(std::string(reinterpret_cast<const char*>(rec.validity.data()),
                                            rec.validity.size()));
        fv->set_quality_status(to_proto_quality(rec.quality));
        fv->set_venue_mask(rec.venue_mask);
        fv->set_missing_venue_mask(rec.missing_venue_mask);
        fv->set_confidence(rec.confidence);
        fv->set_computed_at_ms(rec.computed_at_ms);
        const auto subject = messaging::topology::cross_venue_feature_subject(symbol, Timeframe::m1);
        const std::string msg_id = "xvf:" + std::to_string(rec.canonical_pair_id) + ":" +
                                   std::to_string(rec.bar_open_ms);
        // JetStream with a deterministic id: the DB row is already committed
        // (source of truth), and dedup keeps a re-swept bar from duplicating the
        // event stream. Best-effort short wait; the periodic sweep re-emits.
        (void)nats.js_publish(subject, messaging::serialize(env), msg_id, 2000);
    }
    // Cross-venue quality.
    {
        auto env = base_envelope(rec.canonical_pair_id, symbol, "quality.cross_venue");
        auto* qv = env.mutable_cross_venue_quality();
        qv->set_canonical_pair_id(rec.canonical_pair_id);
        qv->set_timeframe(::alttech::v1::TF_1M);
        qv->set_bar_open_time_ms(rec.bar_open_ms);
        qv->set_quality_status(to_proto_quality(rec.quality));
        qv->set_venue_mask(rec.venue_mask);
        qv->set_missing_venue_mask(rec.missing_venue_mask);
        qv->set_stale_venue_mask(rec.stale_venue_mask);
        qv->set_desynced_venue_mask(rec.desynced_venue_mask);
        qv->set_confidence(rec.confidence);
        for (const auto& r : rec.quality_reasons) qv->add_quality_reasons(r);
        qv->set_observed_at_ms(rec.computed_at_ms);
        const auto subject = messaging::topology::cross_venue_quality_subject(symbol, Timeframe::m1);
        const std::string msg_id = "xvq:" + std::to_string(rec.canonical_pair_id) + ":" +
                                   std::to_string(rec.bar_open_ms);
        (void)nats.js_publish(subject, messaging::serialize(env), msg_id, 2000);
    }
    // Lifecycle event.
    const char* kind = rec.quality == CrossVenueQuality::invalid ? "invalid"
                       : rec.quality == CrossVenueQuality::strict_valid ? "ready" : "degraded";
    {
        auto env = base_envelope(rec.canonical_pair_id, symbol,
                                 std::string("evt.cross_venue.") + kind);
        const auto subject = messaging::topology::cross_venue_event_subject(symbol, kind);
        const std::string msg_id = "xve:" + std::to_string(rec.canonical_pair_id) + ":" +
                                   std::to_string(rec.bar_open_ms) + ":" + kind;
        (void)nats.js_publish(subject, messaging::serialize(env), msg_id, 2000);
    }
}

} // namespace

namespace alttech::modules {

Result<void> run_cross_venue(const YAML::Node& config, persistence::PgPool& pool,
                             messaging::NatsClient& nats,
                             application::ModuleSupervisor& supervisor) {
    const Params params = parse_params(config);
    auto& shutdown = ShutdownToken::instance();

    if (!params.enabled) {
        log::info("cross_venue_disabled", {{"detail", "module present but idle by config"}});
        supervisor.mark_ready(kModuleName);
        while (!shutdown.wait_for(std::chrono::seconds(5))) {}
        return {};
    }

    // Fail-closed on schema/contract integrity.
    if (auto ok = store::validate_schema(pool); !ok) return std::unexpected(ok.error());
    auto set_id = store::ensure_cross_venue_feature_set(pool);
    if (!set_id) return std::unexpected(set_id.error());

    auto universe = store::load_eligible_universe(pool);
    if (!universe) return std::unexpected(universe.error());
    log::info("cross_venue_universe_loaded",
              {{"eligible_pairs", static_cast<int64_t>(universe->size())}});

    // Live universe reaction (best-effort; the periodic reload is authoritative).
    std::atomic<bool> universe_dirty{false};
    auto sub = nats.subscribe(std::string(messaging::topology::kUniverseSubject),
                              [&universe_dirty](messaging::Msg) { universe_dirty.store(true); });
    // A universe subscription failure is not fatal: the periodic DB reload covers it.
    if (!sub) {
        log::warn("cross_venue_universe_sub_failed", {{"message", sub.error().message}});
    }

    supervisor.mark_ready(kModuleName);
    log::info("cross_venue_ready", {{"method", static_cast<int>(params.method)}});

    int64_t last_universe_reload = timeutil::now_ms();
    // The liveness lease covers one PAIR, not one whole sweep.
    //
    // A sweep runs five queries per pair, and on a box under IO pressure its
    // tail is nothing like its median: measured here at a 57 s median but a
    // 514 s maximum, against a 240 s lease plus the 120 s watchdog grace. So
    // systemd SIGABRTed a service that was making steady progress -- 42 times
    // in four days, each kill costing a minute of feature vectors across every
    // instrument in flight, which is what the hourly holes in the vector
    // coverage actually were.
    //
    // Beating once per pair keeps the watchdog's real purpose intact: a sweep
    // wedged on a single pair still stops beaconing and is still force-restarted.
    // What it no longer does is mistake slow for stuck.
    const int64_t liveness_budget_ms = params.cycle_interval_sec * 4000 + 120'000;
    while (!shutdown.stop_requested()) {
        supervisor.heartbeat(kModuleName, liveness_budget_ms);
        int64_t computed = 0, degraded = 0, invalid = 0, consensus_zone_count = 0;
        int64_t with_consensus_price = 0;
        for (const auto& pair : *universe) {
            if (shutdown.stop_requested()) break;
            supervisor.heartbeat(kModuleName, liveness_budget_ms);
            // Find the venue symbol for subjects (Bitget's symbol when present).
            std::string symbol = pair.base_asset + pair.quote_asset;
            for (const auto& v : pair.venues) {
                if (v.venue == Venue::bitget) { symbol = v.exchange_symbol; break; }
            }

            auto bar = store::next_bar_to_compute(pool, pair, params.bar_watermark_ms,
                                                  static_cast<int>(params.lookback_min));
            if (!bar) { // transient DB error -> degrade, keep the process alive
                log::warn("cross_venue_scan_error",
                          {{"pair", pair.canonical_pair_id}, {"message", bar.error().message}});
                continue;
            }
            if (!*bar) continue; // nothing new for this pair

            // A fresh bar waits for its four venue regimes (regime_readiness.hpp):
            // computed before them, it would never get a global regime.
            auto venue_regimes = store::load_venue_regimes(pool, pair.canonical_pair_id, **bar);
            if (venue_regimes &&
                wait_for_venue_regimes(venue_regimes->size(), **bar, timeutil::now_ms())) {
                continue;   // the next pass computes it, with the vote complete
            }

            auto samples = store::load_window(pool, pair, **bar);
            if (!samples) {
                log::warn("cross_venue_window_error",
                          {{"pair", pair.canonical_pair_id}, {"message", samples.error().message}});
                continue;
            }

            // Bounded per-venue return series for lead-lag (aligned 1m grid).
            std::vector<store::ReturnSeries> rs_storage;
            std::vector<VenueSeries> series;
            if (auto rss = store::load_return_series(pool, pair, **bar,
                                                     static_cast<int>(params.return_series_len))) {
                rs_storage = std::move(*rss);
                for (const auto& r : rs_storage) {
                    VenueSeries vs;
                    vs.venue = r.venue;
                    vs.returns = std::span<const double>(r.returns);
                    for (const auto& s : *samples) {
                        if (s.venue == r.venue) { vs.clock_offset_ms = s.clock_offset_ms; break; }
                    }
                    series.push_back(vs);
                }
            }

            auto rec = compute_record(pair.canonical_pair_id, **bar, *set_id, *samples, series,
                                      params);
            if (auto w = store::write_cross_venue(pool, rec); !w) {
                log::warn("cross_venue_write_error",
                          {{"pair", pair.canonical_pair_id}, {"message", w.error().message}});
                continue;
            }
            publish_outputs(nats, symbol, rec);

            // The consensus weights computed above are reused for the regime
            // vote and the zone clustering, so one venue's quality has the same
            // influence on every cross-venue product of this window.
            const auto weight_of = [&rec](Venue venue) {
                for (const auto& c : rec.contributions) {
                    if (c.venue == venue) return c.included ? c.weight : 0.0;
                }
                return 0.0;
            };
            const double primary_mid = [&samples] {
                for (const auto& s : *samples) {
                    if (is_primary(s.venue) && s.mid_price) return *s.mid_price;
                }
                return 0.0;
            }();

            if (auto& regimes = venue_regimes) {
                for (auto& r : *regimes) r.weight = weight_of(r.venue);
                const auto global = aggregate_regimes(*regimes);
                if (auto w = store::write_global_regime(pool, pair.canonical_pair_id, **bar,
                                                        global, rec.quality);
                    !w) {
                    log::warn("global_regime_write_error",
                              {{"pair", pair.canonical_pair_id},
                               {"message", w.error().message}});
                } else {
                    publish_global_regime(nats, symbol, rec, global);
                }
            }

            if (auto zones = store::load_venue_zones(pool, pair.canonical_pair_id)) {
                for (auto& z : *zones) {
                    z.weight = weight_of(z.venue);
                    for (const auto& s : *samples) {
                        if (s.venue == z.venue && s.mid_price) {
                            z.venue_mid = *s.mid_price;
                            break;
                        }
                    }
                }
                const auto consensus_zones =
                    build_consensus_zones(*zones, params.min_consensus_venues);
                if (auto w = store::write_consensus_zones(
                        pool, pair.canonical_pair_id, consensus_zones, primary_mid,
                        rec.consensus_mid.value_or(0.0), rec.confidence, rec.quality,
                        params.zone_stale_after_sec);
                    !w) {
                    log::warn("consensus_zone_write_error",
                              {{"pair", pair.canonical_pair_id},
                               {"message", w.error().message}});
                } else {
                    publish_consensus_zones(nats, symbol, rec, consensus_zones, primary_mid);
                    consensus_zone_count += static_cast<int64_t>(consensus_zones.size());
                }
            }

            ++computed;
            if (rec.consensus_mid) ++with_consensus_price;
            if (rec.quality == CrossVenueQuality::invalid) ++invalid;
            else if (rec.quality != CrossVenueQuality::strict_valid) ++degraded;
        }
        if (computed > 0) {
            log::info("cross_venue_sweep", {{"computed", computed},
                                            {"with_consensus_price", with_consensus_price},
                                            {"degraded", degraded},
                                            {"invalid", invalid},
                                            {"consensus_zones", consensus_zone_count}});
            // A sweep can report a full set of "computed" records and still have
            // agreed on nothing: if every venue is excluded from the estimator
            // the record is written with no consensus price, no consensus
            // funding and no mark/index — which is what a consumer actually
            // needs. That state produced a mark_price of 0 in every downstream
            // decision context while this line still read computed=27,
            // degraded=0, invalid=0, so it must be reported on its own.
            if (with_consensus_price == 0) {
                log::warn("cross_venue_no_consensus",
                          {{"computed", computed},
                           {"message", "every venue was excluded from the consensus estimator; "
                                       "check cross_venue_components.exclude_reason"}});
            }
        }

        // Periodic authoritative universe reload (~5 min or on live change).
        const int64_t now = timeutil::now_ms();
        if (universe_dirty.exchange(false) || now - last_universe_reload > 300'000) {
            if (auto u = store::load_eligible_universe(pool)) {
                *universe = std::move(*u);
                last_universe_reload = now;
            }
        }

        if (shutdown.wait_for(std::chrono::seconds(params.cycle_interval_sec))) break;
    }

    log::info("cross_venue_stopped", {});
    return {};
}

} // namespace alttech::modules
