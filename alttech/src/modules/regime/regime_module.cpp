// Primary-venue market-regime module: deterministic regime classification for
// Bitget on 15m and 1h.
//
// Every axis is expressed in units that are comparable across instruments and
// timeframes, so no threshold depends on an asset's price or tick size:
//
//   trend_strength      ADX(14) / 50, capped at 1. Wilder's original reading of
//                       ADX is: below 20 no trend, 20-25 emerging, above 25
//                       trending, above 50 very strong.
//   volatility_regime   empirical percentile of Bollinger bandwidth over the
//                       trailing 14 days of the same instrument and timeframe,
//                       bounded by the classified bar (never future data).
//   volume_regime       volume z-score over 20 bars, squashed to [0,1] with
//                       +/-3 sigma saturation.
//   momentum_persistence  Kaufman efficiency ratio over 20 bars: 0 is pure
//                       noise, 1 is a perfectly directional move.
//
// The decision boundaries between regimes are deployment parameters, not
// universal constants; they live in the module configuration and the defaults
// below are the conventional readings of each axis.
//
// Snapshots and transition events are persisted; notification events are
// published on alttech.regime.venue.bitget.<sym>.<tf>.v1.

#include "alttech/application/module_supervisor.hpp"
#include "venue_sweep.hpp"
#include "alttech/modules/regime.hpp"

#include "alttech/common/shutdown.hpp"
#include "alttech/config/config.hpp"
#include "alttech/logging/logger.hpp"
#include "alttech/messaging/codec.hpp"
#include "alttech/messaging/nats_client.hpp"
#include "alttech/messaging/topology.hpp"
#include "alttech/persistence/pg.hpp"
#include "alttech/persistence/txn_log.hpp"
#include "alttech/talib_features/types.hpp"
#include "alttech/time/time.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <map>
#include <stdexcept>
#include <string_view>

using namespace alttech;
using json = nlohmann::json;

namespace alttech::modules {
namespace {

constexpr std::string_view kModuleName = "regime";
constexpr const char* kAlgoVersion = "venue-regime-1";
// Liveness slack on top of the idle wait: an upper bound on the work for ONE
// instrument (two timeframe classifications, each a bounded set of indexed
// queries) with room for a loaded database. Progress renews the lease, so this
// bounds how long a genuinely wedged worker can go unnoticed.
constexpr int64_t kProgressLeaseMs = 180'000;
constexpr const char* kFeatureEngineVersion = talib_features::kEngineVersion;
constexpr int kFeatureSetVersion = talib_features::kCoreSetVersion;

// Decision boundaries between regimes, in normalized axis units. Defaults are
// the conventional readings of each axis; every value is configurable under
// modules.regime.thresholds.
struct Thresholds {
    double adx_trending = 25.0;         // Wilder: a trend is established above 25
    double low_liquidity_volume = 0.15; // volume_regime, i.e. about -2 sigma
    double chaotic_efficiency = 0.12;   // efficiency ratio: essentially directionless
    double chaotic_volatility = 0.85;   // volatility percentile
    double expansion_volatility = 0.90; // volatility percentile
    double breakout_volume_z = 1.5;     // volume z-score confirming the expansion
    double compression_volatility = 0.12;
};

Thresholds parse_thresholds(const YAML::Node& module_config) {
    const YAML::Node node = module_config["thresholds"];
    Thresholds t;
    t.adx_trending = config::opt_double(node, "adx_trending", t.adx_trending);
    t.low_liquidity_volume =
        config::opt_double(node, "low_liquidity_volume", t.low_liquidity_volume);
    t.chaotic_efficiency = config::opt_double(node, "chaotic_efficiency", t.chaotic_efficiency);
    t.chaotic_volatility = config::opt_double(node, "chaotic_volatility", t.chaotic_volatility);
    t.expansion_volatility =
        config::opt_double(node, "expansion_volatility", t.expansion_volatility);
    t.breakout_volume_z = config::opt_double(node, "breakout_volume_z", t.breakout_volume_z);
    t.compression_volatility =
        config::opt_double(node, "compression_volatility", t.compression_volatility);
    return t;
}

Result<void> validate_thresholds(const Thresholds& t) {
    const auto unit = [](double v) { return v > 0.0 && v < 1.0; };
    if (!(t.adx_trending > 0.0 && t.adx_trending < 100.0) || !unit(t.low_liquidity_volume) ||
        !unit(t.chaotic_efficiency) || !unit(t.chaotic_volatility) ||
        !unit(t.expansion_volatility) || !unit(t.compression_volatility) ||
        !(t.breakout_volume_z > 0.0 && t.breakout_volume_z < 10.0) ||
        t.compression_volatility >= t.chaotic_volatility ||
        t.chaotic_volatility > t.expansion_volatility) {
        return fail(ErrorCode::config_error,
                    "regime.thresholds are outside their supported ranges or not ordered "
                    "(compression < chaotic <= expansion)");
    }
    return {};
}

struct RegimeInput {
    int64_t bar_ms = 0;
    int64_t candle_revision = 0;
    double adx = 0, ema20 = 0, ema50 = 0, ema200 = 0, atr = 0, bb_width = 0;
    double vol_z = 0, eff_ratio = 0, close = 0;
    bool has_eff_ratio = false;
    std::string projection_updated_at;
    std::string feature_computed_at;
    bool complete = false;
};

struct RegimeResult {
    int64_t bar_open_time_ms = 0;
    std::string regime = "UNDEFINED";
    double confidence = 0;
    json axes;
};

::alttech::v1::Timeframe wire_timeframe(std::string_view label) {
    const auto timeframe = tf_from_label(label);
    return timeframe ? static_cast<::alttech::v1::Timeframe>(tf_minutes(*timeframe))
                     : ::alttech::v1::TIMEFRAME_UNSPECIFIED;
}

::alttech::v1::MarketRegime wire_regime(std::string_view regime) {
    if (regime == "UNDEFINED") return ::alttech::v1::MARKET_REGIME_UNDEFINED;
    if (regime == "LOW_LIQUIDITY") return ::alttech::v1::MARKET_REGIME_LOW_LIQUIDITY;
    if (regime == "CHAOTIC") return ::alttech::v1::MARKET_REGIME_CHAOTIC;
    if (regime == "TREND_UP") return ::alttech::v1::MARKET_REGIME_TREND_UP;
    if (regime == "TREND_DOWN") return ::alttech::v1::MARKET_REGIME_TREND_DOWN;
    if (regime == "BREAKOUT") return ::alttech::v1::MARKET_REGIME_BREAKOUT;
    if (regime == "VOLATILITY_EXPANSION") {
        return ::alttech::v1::MARKET_REGIME_VOLATILITY_EXPANSION;
    }
    if (regime == "VOLATILITY_COMPRESSION") {
        return ::alttech::v1::MARKET_REGIME_VOLATILITY_COMPRESSION;
    }
    if (regime == "RANGE") return ::alttech::v1::MARKET_REGIME_RANGE;
    return ::alttech::v1::MARKET_REGIME_UNSPECIFIED;
}

void fill_regime_axes(const json& axes, ::alttech::v1::RegimeAxes* output) {
    output->set_trend_strength(axes.value("trend_strength", 0.0));
    output->set_volatility_regime(axes.value("volatility_regime", 0.0));
    output->set_volume_regime(axes.value("volume_regime", 0.0));
    const auto momentum = axes.find("momentum_persistence");
    if (momentum != axes.end() && !momentum->is_null()) {
        output->set_momentum_persistence(momentum->get<double>());
    }
    output->set_liquidity_quality(axes.value("liquidity_quality", 0.0));
    output->set_data_quality(axes.value("data_quality", 0.0));
    output->set_source_bar_revision(axes.value("source_bar_revision", int64_t{0}));
    output->set_source_projection_updated_at(
        axes.value("source_projection_updated_at", std::string{}));
    output->set_source_feature_computed_at(
        axes.value("source_feature_computed_at", std::string{}));
}

class RegimeService {
public:
    RegimeService(persistence::PgPool& pool, messaging::NatsClient& nats, Thresholds thresholds)
        : pool_(pool), nats_(nats), thresholds_(thresholds) {}

    // `on_progress` is invoked after every instrument so the caller can renew
    // its liveness lease from real progress. A cycle legitimately takes longer
    // than one cycle interval when the universe is large or PostgreSQL is
    // loaded; declaring the module dead for that reason force-restarted a
    // perfectly healthy process before it could ever finish a cycle.
    void run_cycle(const std::function<void()>& on_progress = {}) {
        std::vector<std::pair<int64_t, std::string>> instruments;
        auto res = pool_.with_txn([&](pqxx::work& txn) {
            const auto rows = txn.exec(
                "SELECT ei.instrument_id, ei.exchange_symbol "
                "FROM global_observation_universe u "
                "JOIN canonical_pair_exchange_map m "
                "  ON m.canonical_pair_id = u.canonical_pair_id AND m.exchange_id = 1 "
                "JOIN exchange_instruments ei ON ei.id = m.exchange_instrument_id "
                "WHERE u.eligible AND ei.is_whitelisted AND ei.status = 'active' "
                "ORDER BY ei.exchange_symbol");
            for (const auto& r : rows) {
                instruments.emplace_back(r[0].as<int64_t>(), r[1].as<std::string>());
            }
        });
        if (!res) {
            log::error("regime_instruments_failed", {{"message", res.error().message}});
            return;
        }
        int snapshots = 0;
        int no_source = 0;
        for (const auto& [iid, sym] : instruments) {
            if (ShutdownToken::instance().stop_requested()) return;
            if (on_progress) on_progress();
            std::map<std::string, RegimeResult> per_tf;
            for (const char* tf : {"15m", "1h"}) {
                auto input = load_input(iid, tf);
                if (!input) { ++no_source; continue; }
                auto classified = classify(*input, iid, tf);
                if (!classified) continue;
                if (!persist(iid, sym, tf, *input, *classified)) continue;
                per_tf[tf] = *classified;
                ++snapshots;
            }
            if (!per_tf.empty()) publish_combined(iid, sym, per_tf);
        }
        // A cycle that classifies nothing is a failure to report, not a quiet
        // success: it means no (instrument, timeframe) had a VALID feature
        // vector whose freshness chain matched its candle revision.
        if (snapshots == 0 && !instruments.empty()) {
            log::warn("regime_cycle_produced_nothing",
                      {{"instruments", static_cast<int64_t>(instruments.size())},
                       {"missing_source_slots", no_source}});
        }
        log::info("regime_cycle_done", {{"snapshots", snapshots},
                                        {"missing_source_slots", no_source}});
    }

    // One-shot historical backfill: classify every source-valid bar that has no
    // snapshot for the current algorithm version, from `from_ms` forward,
    // using the exact production classifier with bar-relative context (no
    // future data, no transition spam, no NATS publish). Reuses the live code
    // path so backtests can never drift from live regime logic.
    Result<void> run_backfill(int64_t from_ms) {
        std::vector<std::pair<int64_t, std::string>> instruments;
        auto res = pool_.with_txn([&](pqxx::work& txn) {
            const auto rows = txn.exec(
                "SELECT ei.instrument_id, ei.exchange_symbol "
                "FROM global_observation_universe u "
                "JOIN canonical_pair_exchange_map m "
                "  ON m.canonical_pair_id = u.canonical_pair_id AND m.exchange_id = 1 "
                "JOIN exchange_instruments ei ON ei.id = m.exchange_instrument_id "
                "WHERE u.eligible AND ei.is_whitelisted AND ei.status = 'active' "
                "ORDER BY ei.exchange_symbol");
            for (const auto& r : rows) {
                instruments.emplace_back(r[0].as<int64_t>(), r[1].as<std::string>());
            }
        });
        if (!res) {
            log::error("regime_backfill_instruments_failed", {{"message", res.error().message}});
            return std::unexpected(res.error());
        }
        int64_t total = 0;
        for (const auto& [iid, sym] : instruments) {
            if (ShutdownToken::instance().stop_requested()) return {};
            for (const char* tf : {"15m", "1h"}) {
                std::vector<int64_t> bars;
                auto loaded = pool_.with_txn([&](pqxx::work& txn) {
                    txn.exec("SET LOCAL TIME ZONE 'UTC'");
                    const auto rows = txn.exec_params(
                        "SELECT (extract(epoch FROM p.bar_open_time)*1000)::bigint "
                        "FROM alttech.venue_feature_projection p "
                        "JOIN market_candles_agg c ON c.exchange_id = p.exchange_id "
                        "  AND c.instrument_id = p.canonical_pair_id AND c.timeframe = p.timeframe "
                        "  AND c.open_time = p.bar_open_time "
                        // venue_class is the LIST partition key and the leading PK column. The
                // regime module is primary-venue only, so naming it prunes the
                // reference partition (three quarters of the table) instead of
                // scanning it on every probe.
                "JOIN LATERAL (SELECT v.computed_at FROM alttech.venue_feature_vectors v "
                " "
                        " JOIN alttech.feature_set_versions fs ON fs.id = v.feature_set_version_id "
                        " WHERE v.venue_class = 'bitget' AND v.exchange_id = p.exchange_id "
                        " AND v.canonical_pair_id = p.canonical_pair_id AND v.timeframe = p.timeframe "
                        " AND v.bar_open_time = p.bar_open_time "
                        " AND v.bar_revision = c.revision AND v.status = 'VALID' "
                        " AND fs.name = 'talib-core' AND fs.version = $4 AND fs.frozen "
                        " AND fs.engine_version = $5 AND v.engine_version = $5 "
                        " AND v.computed_at >= c.updated_at "
                        " AND p.updated_at >= v.computed_at "
                        " ORDER BY v.computed_at DESC LIMIT 1) source ON true "
                        "WHERE p.exchange_id = 1 AND p.canonical_pair_id = $1 AND p.timeframe = $2 "
                        "AND c.is_closed AND c.quality_status = 'ok' "
                        "AND p.bar_open_time >= to_timestamp($3::bigint/1000.0) "
                        "AND NOT EXISTS (SELECT 1 FROM alttech.venue_regimes s "
                        "  WHERE s.exchange_id = 1 AND s.canonical_pair_id = p.canonical_pair_id "
                        "  AND s.timeframe = p.timeframe AND s.bar_open_time = p.bar_open_time "
                        "  AND s.algorithm_version = $6) "
                        "ORDER BY p.bar_open_time",
                        iid, tf, from_ms, kFeatureSetVersion,
                        std::string(kFeatureEngineVersion), std::string(kAlgoVersion));
                    for (const auto& r : rows) bars.push_back(r[0].as<int64_t>());
                });
                if (!loaded) {
                    log::error("regime_backfill_source_load_failed",
                               {{"symbol", sym}, {"timeframe", tf},
                                {"message", loaded.error().message}});
                    return std::unexpected(loaded.error());
                }
                int64_t done = 0;
                for (int64_t bar_ms : bars) {
                    if (ShutdownToken::instance().stop_requested()) return {};
                    auto input = load_input(iid, tf, bar_ms);
                    if (!input) continue;
                    auto classified = classify(*input, iid, tf);
                    if (!classified) continue;
                    bool source_current = false;
                    auto w = pool_.with_txn([&](pqxx::work& txn) {
                        source_current = current_source_exists(txn, iid, tf, *input);
                        if (!source_current) return;
                        txn.exec_params(
                            "INSERT INTO alttech.venue_regimes (exchange_id, canonical_pair_id, "
                            "timeframe, bar_open_time, regime, confidence, axes, algorithm_version) "
                            "VALUES (1,$1,$2,to_timestamp($3::bigint/1000.0),$4,$5,$6::jsonb,$7) "
                            "ON CONFLICT (exchange_id,canonical_pair_id,timeframe,bar_open_time) "
                            "DO UPDATE SET regime = EXCLUDED.regime, "
                            "confidence = EXCLUDED.confidence, axes = EXCLUDED.axes, "
                            "algorithm_version = EXCLUDED.algorithm_version, computed_at = now()",
                            iid, tf, bar_ms, classified->regime, classified->confidence,
                            classified->axes.dump(), std::string(kAlgoVersion));
                    });
                    if (!w) {
                        log::error("regime_backfill_persist_failed",
                                   {{"symbol", sym}, {"timeframe", tf},
                                    {"bar_open_time_ms", bar_ms},
                                    {"message", w.error().message}});
                        return std::unexpected(w.error());
                    }
                    if (source_current) { ++done; ++total; }
                }
                if (done > 0) {
                    log::info("regime_backfill_shard",
                              {{"symbol", sym}, {"timeframe", tf}, {"filled", done}});
                }
            }
        }
        log::info("regime_backfill_done", {{"filled", total}});
        return {};
    }

private:
    // at_bar_ms == 0 loads the latest bar (live); a positive value loads that
    // exact bar (backfill).
    std::optional<RegimeInput> load_input(int64_t iid, const std::string& tf,
                                          int64_t at_bar_ms = 0) {
        RegimeInput in;
        auto res = pool_.with_txn([&](pqxx::work& txn) {
            txn.exec("SET LOCAL TIME ZONE 'UTC'");
            const std::string sel =
                "WITH selected AS (SELECT p.* FROM alttech.venue_feature_projection p "
                "WHERE p.exchange_id = 1 AND p.canonical_pair_id = $1 AND p.timeframe = $2 ";
            const std::string bound =
                at_bar_ms > 0
                    ? "AND p.bar_open_time = to_timestamp($3::bigint/1000.0) "
                    : "AND $3::bigint = 0 ORDER BY p.bar_open_time DESC LIMIT 1 ";
            const std::string gated =
                ") SELECT (extract(epoch FROM p.bar_open_time) * 1000)::bigint, p.adx_14, "
                "p.ema_20, p.ema_50, p.ema_200, p.atr_14, p.bb_width_20, p.volume_zscore_20, "
                "p.efficiency_ratio_20, c.close::float8, c.revision, "
                "p.updated_at::text, source.computed_at::text "
                "FROM selected p "
                "JOIN market_candles_agg c ON c.exchange_id = p.exchange_id "
                "  AND c.instrument_id = p.canonical_pair_id AND c.timeframe = p.timeframe "
                "  AND c.open_time = p.bar_open_time "
                // venue_class is the LIST partition key and the leading PK column. The
                // regime module is primary-venue only, so naming it prunes the
                // reference partition (three quarters of the table) instead of
                // scanning it on every probe.
                "JOIN LATERAL (SELECT v.computed_at FROM alttech.venue_feature_vectors v "
                " "
                " JOIN alttech.feature_set_versions fs ON fs.id = v.feature_set_version_id "
                " WHERE v.venue_class = 'bitget' AND v.exchange_id = p.exchange_id "
                " AND v.canonical_pair_id = p.canonical_pair_id AND v.timeframe = p.timeframe "
                " AND v.bar_open_time = p.bar_open_time "
                " AND v.bar_revision = c.revision AND v.status = 'VALID' "
                " AND fs.name = 'talib-core' AND fs.version = $4 AND fs.frozen "
                " AND fs.engine_version = $5 AND v.engine_version = $5 "
                " AND v.computed_at >= c.updated_at "
                " AND p.updated_at >= v.computed_at "
                " ORDER BY v.computed_at DESC LIMIT 1) source ON true "
                "WHERE c.is_closed AND c.quality_status = 'ok' ";
            const auto rows =
                at_bar_ms > 0
                    ? txn.exec_params(sel + bound + gated, iid, tf, at_bar_ms,
                                      kFeatureSetVersion,
                                      std::string(kFeatureEngineVersion))
                    : txn.exec_params(sel + bound + gated, iid, tf, int64_t{0},
                                      kFeatureSetVersion,
                                      std::string(kFeatureEngineVersion));
            if (rows.empty()) return;
            const auto& r = rows[0];
            in.bar_ms = r[0].as<int64_t>();
            in.adx = r[1].is_null() ? 0 : r[1].as<double>();
            in.ema20 = r[2].is_null() ? 0 : r[2].as<double>();
            in.ema50 = r[3].is_null() ? 0 : r[3].as<double>();
            in.ema200 = r[4].is_null() ? 0 : r[4].as<double>();
            in.atr = r[5].is_null() ? 0 : r[5].as<double>();
            in.bb_width = r[6].is_null() ? 0 : r[6].as<double>();
            in.vol_z = r[7].is_null() ? 0 : r[7].as<double>();
            in.has_eff_ratio = !r[8].is_null();
            in.eff_ratio = in.has_eff_ratio ? r[8].as<double>() : 0.0;
            in.close = r[9].as<double>();
            in.candle_revision = r[10].as<int64_t>();
            in.projection_updated_at = r[11].as<std::string>();
            in.feature_computed_at = r[12].as<std::string>();
            in.complete = true;
            // efficiency_ratio_20 has no TA-Lib equivalent and is therefore
            // optional. Every other classifier input remains fail-closed.
            for (int column = 1; column <= 7; ++column) {
                if (r[column].is_null()) in.complete = false;
            }
            if (r[9].is_null()) in.complete = false;
            in.complete = in.complete && std::isfinite(in.adx) && std::isfinite(in.ema20) &&
                          std::isfinite(in.ema50) && std::isfinite(in.ema200) &&
                          std::isfinite(in.atr) && std::isfinite(in.bb_width) &&
                          std::isfinite(in.vol_z) && std::isfinite(in.close) &&
                          (!in.has_eff_ratio || std::isfinite(in.eff_ratio));
        });
        if (!res) {
            log::error("regime_input_load_failed",
                       {{"instrument_id", iid}, {"timeframe", tf},
                        {"message", res.error().message}});
            return std::nullopt;
        }
        if (in.bar_ms == 0) return std::nullopt;
        return in;
    }

    std::optional<RegimeResult> classify(const RegimeInput& in, int64_t iid,
                                         const std::string& tf) {
        RegimeResult r;
        r.bar_open_time_ms = in.bar_ms;
        // BB width percentile vs recent history (volatility axis).
        double bbw_pct = 0.5;
        auto percentile = pool_.with_txn([&](pqxx::work& txn) {
            // The BB-width percentile is relative to the 14 days preceding the
            // bar being classified (bounded by the bar itself, never future
            // data). For the live latest bar this equals now()-14d..now(); for
            // backfill it makes every historical classification leakage-safe.
            // The per-bar source-freshness chain is deliberately NOT re-verified
            // across the history window. A projection row only exists because a
            // VALID vector of the pinned engine/set produced it, so re-proving
            // that for all ~1300 historical bars added a LATERAL probe into the
            // largest table in the schema for every one of them: the query ran
            // 4-10 s, was executed 54 times a cycle, and dominated both the
            // regime cycle time and the PostgreSQL log. The strict chain still
            // gates the bar being CLASSIFIED — see load_input() — which is where
            // it changes an answer. Here it only shapes a percentile.
            const auto rows = txn.exec_params(
                "WITH valid_history AS ("
                " SELECT h.bb_width_20 FROM alttech.venue_feature_projection h "
                " JOIN market_candles_agg c ON c.exchange_id = h.exchange_id "
                " AND c.instrument_id = h.canonical_pair_id AND c.timeframe = h.timeframe "
                " AND c.open_time = h.bar_open_time "
                " WHERE h.exchange_id = 1 AND h.canonical_pair_id = $1 AND h.timeframe = $2 "
                " AND c.is_closed AND c.quality_status = 'ok' "
                " AND h.bb_width_20 IS NOT NULL "
                " AND h.bar_open_time > to_timestamp($4::bigint/1000.0) - interval '14 days' "
                " AND h.bar_open_time <= to_timestamp($4::bigint/1000.0)) "
                "SELECT count(*) FILTER (WHERE bb_width_20 <= $3)::float8 / "
                "GREATEST(count(*), 1) FROM valid_history",
                iid, tf, in.bb_width, in.bar_ms);
            if (!rows.empty()) bbw_pct = rows[0][0].as<double>();
        });
        if (!percentile) {
            log::error("regime_percentile_failed",
                       {{"instrument_id", iid}, {"timeframe", tf},
                        {"message", percentile.error().message}});
            return std::nullopt;
        }

        // ADX 50 is Wilder's "very strong trend" reading and is used as the
        // saturation point of the [0,1] trend axis.
        const double trend_strength = std::min(1.0, in.adx / 50.0);
        const bool aligned_up = in.close > in.ema20 && in.ema20 > in.ema50 && in.ema50 > in.ema200;
        const bool aligned_dn = in.close < in.ema20 && in.ema20 < in.ema50 && in.ema50 < in.ema200;
        const double vol_regime = bbw_pct;
        // Volume z-score squashed to [0,1]; +/-3 sigma saturates the axis.
        const double volume_regime = std::clamp(0.5 + in.vol_z / 6.0, 0.0, 1.0);
        const double liquidity = volume_regime; // traded volume is the liquidity proxy

        r.axes = {{"trend_strength", trend_strength},
                  {"volatility_regime", vol_regime},
                  {"volume_regime", volume_regime},
                  {"momentum_persistence",
                   in.has_eff_ratio ? json(in.eff_ratio) : json(nullptr)},
                  {"liquidity_quality", liquidity},
                  {"data_quality", in.complete ? 1.0 : 0.0},
                  // Additive provenance allows readers and operators to
                  // recognize a snapshot calculated from a superseded candle
                  // revision without changing the shared SQL schema.
                  {"source_bar_revision", in.candle_revision},
                  {"source_projection_updated_at", in.projection_updated_at},
                  {"source_feature_computed_at", in.feature_computed_at}};

        if (!in.complete) {
            r.regime = "UNDEFINED";
            r.confidence = 0.2;
            return r;
        }
        if (volume_regime < thresholds_.low_liquidity_volume) {
            r.regime = "LOW_LIQUIDITY";
            r.confidence = 0.6;
        } else if (in.has_eff_ratio && in.eff_ratio < thresholds_.chaotic_efficiency &&
                   vol_regime > thresholds_.chaotic_volatility) {
            r.regime = "CHAOTIC";
            r.confidence = 0.55;
        } else if (in.adx >= thresholds_.adx_trending && aligned_up) {
            r.regime = "TREND_UP";
            r.confidence = std::min(0.95, 0.5 + trend_strength / 2);
        } else if (in.adx >= thresholds_.adx_trending && aligned_dn) {
            r.regime = "TREND_DOWN";
            r.confidence = std::min(0.95, 0.5 + trend_strength / 2);
        } else if (vol_regime > thresholds_.expansion_volatility &&
                   in.vol_z > thresholds_.breakout_volume_z) {
            r.regime = "BREAKOUT";
            r.confidence = 0.6;
        } else if (vol_regime > thresholds_.expansion_volatility) {
            r.regime = "VOLATILITY_EXPANSION";
            r.confidence = 0.6;
        } else if (vol_regime < thresholds_.compression_volatility) {
            r.regime = "VOLATILITY_COMPRESSION";
            r.confidence = 0.65;
        } else {
            r.regime = "RANGE";
            r.confidence = in.has_eff_ratio
                               ? std::min(0.9, 0.45 + (1.0 - in.eff_ratio) / 2)
                               : 0.45;
        }
        return r;
    }

    bool current_source_exists(pqxx::work& txn, int64_t iid, const std::string& tf,
                               const RegimeInput& input) const {
        const auto rows = txn.exec_params(
            "SELECT EXISTS (SELECT 1 FROM alttech.venue_feature_projection p "
            "JOIN market_candles_agg c ON c.exchange_id = p.exchange_id "
            "AND c.instrument_id = p.canonical_pair_id AND c.timeframe = p.timeframe "
            "AND c.open_time = p.bar_open_time "
            "JOIN alttech.venue_feature_vectors v ON v.exchange_id = p.exchange_id "
            "AND v.canonical_pair_id = p.canonical_pair_id AND v.timeframe = p.timeframe "
            "AND v.bar_open_time = p.bar_open_time AND v.bar_revision = c.revision "
            "JOIN alttech.feature_set_versions fs ON fs.id = v.feature_set_version_id "
            "WHERE p.exchange_id = 1 AND p.canonical_pair_id = $1 AND p.timeframe = $2 "
            "AND p.bar_open_time = to_timestamp($3::bigint / 1000.0) "
            "AND c.is_closed AND c.quality_status = 'ok' AND c.revision = $4 "
            "AND p.updated_at = $5::timestamptz AND v.computed_at = $6::timestamptz "
            "AND v.status = 'VALID' AND fs.name = 'talib-core' "
            "AND fs.version = $7 AND fs.frozen AND fs.engine_version = $8 "
            "AND v.engine_version = $8 AND v.computed_at >= c.updated_at "
            "AND p.updated_at >= v.computed_at)",
            iid, tf, input.bar_ms, input.candle_revision, input.projection_updated_at,
            input.feature_computed_at, kFeatureSetVersion,
            std::string(kFeatureEngineVersion));
        return !rows.empty() && rows[0][0].as<bool>();
    }

    bool persist(int64_t iid, const std::string& sym, const std::string& tf,
                 const RegimeInput& input, const RegimeResult& r) {
        std::string prev;
        bool source_current = false;
        const bool committed =
            persistence::with_txn_logged(pool_, "regime_persist", [&](pqxx::work& txn) {
            source_current = current_source_exists(txn, iid, tf, input);
            if (!source_current) return;
            const auto p = txn.exec_params(
                "SELECT regime FROM alttech.venue_regimes WHERE exchange_id = 1 "
                "AND canonical_pair_id = $1 AND timeframe = $2 "
                "ORDER BY bar_open_time DESC LIMIT 1",
                iid, tf);
            if (!p.empty()) prev = p[0][0].as<std::string>();
            txn.exec_params(
                "INSERT INTO alttech.venue_regimes (exchange_id, canonical_pair_id, timeframe, "
                "bar_open_time, regime, confidence, axes, algorithm_version) "
                "VALUES (1, $1, $2, to_timestamp($3::bigint / 1000.0), $4, $5, $6::jsonb, $7) "
                "ON CONFLICT (exchange_id, canonical_pair_id, timeframe, bar_open_time) DO UPDATE "
                "SET regime = EXCLUDED.regime, confidence = EXCLUDED.confidence, "
                "axes = EXCLUDED.axes, algorithm_version = EXCLUDED.algorithm_version, "
                "computed_at = now()",
                iid, tf, input.bar_ms, r.regime, r.confidence, r.axes.dump(),
                std::string(kAlgoVersion));
            if (!prev.empty() && prev != r.regime) {
                txn.exec_params(
                    "INSERT INTO alttech.regime_events (scope, exchange_id, canonical_pair_id, "
                    "timeframe, from_regime, to_regime, bar_open_time) "
                    "VALUES ('VENUE', 1, $1, $2, $3, $4, to_timestamp($5::bigint / 1000.0))",
                    iid, tf, prev, r.regime, input.bar_ms);
            }
            });
        if (!committed || !source_current) return false;
        if (prev != r.regime) {
            messaging::EnvelopeMeta meta;
            meta.event_type = "regime.updated";
            meta.instrument_id = iid;
            meta.exchange_symbol = sym;
            meta.exchange_timestamp_ms = input.bar_ms;
            meta.source_service = "alttech-service";
            meta.message_id =
                "rg:" + sym + ":" + tf + ":" + std::to_string(input.bar_ms);
            auto env = messaging::make_envelope(meta);
            auto* snapshot = env.mutable_regime_snapshot();
            snapshot->set_timeframe(wire_timeframe(tf));
            snapshot->set_bar_open_time_ms(input.bar_ms);
            snapshot->set_regime(wire_regime(r.regime));
            if (!prev.empty()) snapshot->set_previous_regime(wire_regime(prev));
            snapshot->set_confidence(r.confidence);
            fill_regime_axes(r.axes, snapshot->mutable_axes());
            snapshot->set_algorithm_version(kAlgoVersion);
            if (auto published = nats_.js_publish(
                    messaging::topology::venue_regime_subject(Venue::bitget, sym,
                                                              *tf_from_label(tf)),
                    messaging::serialize(env), env.message_id());
                !published) {
                log::warn("regime_publish_failed",
                          {{"symbol", sym}, {"timeframe", tf},
                           {"message", published.error().message}});
            }
        }
        return true;
    }

    void publish_combined(int64_t iid, const std::string& sym,
                          const std::map<std::string, RegimeResult>& per_tf) {
        // Combined: higher timeframe dominates on conflict.
        const auto& primary = per_tf.contains("1h") ? per_tf.at("1h") : per_tf.begin()->second;
        messaging::EnvelopeMeta meta;
        meta.event_type = "regime.combined";
        meta.instrument_id = iid;
        meta.exchange_symbol = sym;
        for (const auto& [_, result] : per_tf) {
            meta.exchange_timestamp_ms =
                std::max(meta.exchange_timestamp_ms, result.bar_open_time_ms);
        }
        meta.source_service = "alttech-service";
        meta.message_id = "rgc:" + sym + ":" + std::to_string(timeutil::now_ms() / 60000);
        auto env = messaging::make_envelope(meta);
        auto* combined = env.mutable_combined_regime();
        combined->set_primary_regime(wire_regime(primary.regime));
        for (const auto& [timeframe, result] : per_tf) {
            auto* summary = combined->add_timeframes();
            summary->set_timeframe(wire_timeframe(timeframe));
            summary->set_regime(wire_regime(result.regime));
            summary->set_confidence(result.confidence);
            fill_regime_axes(result.axes, summary->mutable_axes());
            summary->set_bar_open_time_ms(result.bar_open_time_ms);
        }
        combined->set_algorithm_version(kAlgoVersion);
        if (auto published = nats_.js_publish(
                messaging::topology::venue_regime_combined_subject(Venue::bitget, sym),
                messaging::serialize(env), env.message_id());
            !published) {
            log::warn("regime_combined_publish_failed",
                      {{"symbol", sym}, {"message", published.error().message}});
        }
    }

    persistence::PgPool& pool_;
    messaging::NatsClient& nats_;
    Thresholds thresholds_;
};

} // namespace

Result<void> run_regime(const YAML::Node& module_config,
                        persistence::PgPool& pool,
                        messaging::NatsClient& nats,
                        application::ModuleSupervisor& supervisor) {
    const int64_t cycle_sec = config::opt_int(module_config, "cycle_interval_sec", 30);
    if (cycle_sec < 1 || cycle_sec > 3600) {
        log::critical("invalid_regime_service_config",
                      {{"cycle_interval_sec", cycle_sec}});
        Error error{ErrorCode::config_error,
                    "regime config requires cycle_interval_sec in [1,3600]"};
        supervisor.mark_failed(kModuleName, error);
        return std::unexpected(std::move(error));
    }

    if (auto probe = pool.with_txn([](pqxx::work& txn) {
            const auto row = txn.exec1(
                "SELECT to_regclass('exchanges') IS NOT NULL "
                "AND to_regclass('exchange_instruments') IS NOT NULL "
                "AND to_regclass('market_candles_agg') IS NOT NULL "
                "AND to_regclass('alttech.feature_set_versions') IS NOT NULL "
                "AND to_regclass('alttech.venue_feature_vectors') IS NOT NULL "
                "AND to_regclass('alttech.venue_feature_projection') IS NOT NULL "
                "AND to_regclass('alttech.venue_regimes') IS NOT NULL "
                "AND to_regclass('alttech.regime_events') IS NOT NULL");
            if (!row[0].as<bool>()) {
                throw std::runtime_error("required market-regime schema is missing");
            }
        });
        !probe) {
        log::critical("postgres_schema_unavailable", {{"message", probe.error().message}});
        supervisor.mark_failed(kModuleName, probe.error());
        return std::unexpected(probe.error());
    }

    const Thresholds thresholds = parse_thresholds(module_config);
    if (auto valid = validate_thresholds(thresholds); !valid) {
        log::critical("invalid_regime_thresholds", {{"message", valid.error().message}});
        supervisor.mark_failed(kModuleName, valid.error());
        return std::unexpected(valid.error());
    }

    RegimeService service(pool, nats, thresholds);

    // Backfill is a bounded startup phase of this module, not a one-shot
    // process mode. Keep every runtime option in the unified service config.
    const int64_t backfill_days = config::opt_int(module_config, "backfill_days", 0);
    constexpr int64_t kMaxBackfillDays = 36'500;
    if (backfill_days < 0 || backfill_days > kMaxBackfillDays) {
        Error error{ErrorCode::config_error,
                    "regime.backfill_days must be in [0,36500]"};
        log::critical("invalid_regime_backfill_config", {{"message", error.message}});
        supervisor.mark_failed(kModuleName, error);
        return std::unexpected(std::move(error));
    }

    if (backfill_days > 0) {
        constexpr int64_t kDayMs = 86'400'000;
        const int64_t now_ms = timeutil::now_ms();
        const int64_t span_ms = backfill_days * kDayMs;
        const int64_t from_ms = span_ms >= now_ms ? 0 : now_ms - span_ms;
        log::info("regime_backfill_start", {{"from_days", backfill_days}});
        auto backfilled = service.run_backfill(from_ms);
        if (!backfilled) {
            log::critical("regime_backfill_failed",
                          {{"message", backfilled.error().message}});
            supervisor.mark_failed(kModuleName, backfilled.error());
            return std::unexpected(backfilled.error());
        }
        if (ShutdownToken::instance().stop_requested()) {
            log::info("regime_module_stopped_during_backfill", {});
            return {};
        }
        log::info("regime_backfill_live_transition",
                  {{"from_days", backfill_days},
                   {"message", "historical backfill complete; entering live loop"}});
    }

    supervisor.mark_ready(kModuleName);
    log::info("regime_module_ready", {{"timeframes", "15m,1h"},
                                       {"mode", "deterministic"}});
    auto& shutdown = ShutdownToken::instance();
    // Per-venue 1m regimes for all four venues; the global regime consumes
    // them. Joined on every exit path.
    venuelite::SweepThreadJoiner venue_join{
        std::thread(venuelite::run_venue_sweep, std::ref(pool), std::ref(nats),
                    venuelite::store::VenueOutput::regime, module_config)};
    // Liveness is leased from real progress, not from cycle completion. The
    // lease only has to cover the slowest single instrument plus the idle wait
    // between cycles, so a wedged worker is still detected quickly while a slow
    // — but advancing — cycle is not mistaken for a hang.
    const int64_t liveness_budget_ms =
        static_cast<int64_t>(cycle_sec) * 1000 + kProgressLeaseMs;
    const auto beat = [&] { supervisor.heartbeat(kModuleName, liveness_budget_ms); };
    try {
        while (!shutdown.stop_requested()) {
            beat();
            service.run_cycle(beat);
            beat();
            if (shutdown.wait_for(std::chrono::seconds(cycle_sec))) break;
        }
    } catch (const std::exception& e) {
        Error error{ErrorCode::internal_error,
                    std::string("regime module terminated: ") + e.what()};
        log::critical("regime_module_failed", {{"message", error.message}});
        supervisor.mark_failed(kModuleName, error);
        return std::unexpected(std::move(error));
    } catch (...) {
        Error error{ErrorCode::internal_error,
                    "regime module terminated with a non-standard exception"};
        log::critical("regime_module_failed", {{"message", error.message}});
        supervisor.mark_failed(kModuleName, error);
        return std::unexpected(std::move(error));
    }

    log::info("regime_module_stopped", {});
    return {};
}

} // namespace alttech::modules
