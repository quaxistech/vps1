// altspotter config-check: validates the unified service config without printing
// secrets.
//
//   altspotter config-check --config <altspotter.yaml> [--common <common.yaml>]
//
// Checks the shared `common` block (if given) and every recognised component
// section of the unified config. Exit codes: 0 = all valid, 1 = validation
// errors, 2 = usage error.

#include "config/config.hpp"
#include "core/types.hpp"
#include "ops/depth_consistency.hpp"
#include "ops/ops.hpp"

#include <yaml-cpp/yaml.h>

#include <iostream>
#include <algorithm>
#include <string>
#include <vector>

namespace altspotter::ops {

namespace {

struct CheckResult {
    std::string label;
    bool ok = true;
    std::vector<std::string> errors;
};

void require(CheckResult& r, bool cond, const std::string& message) {
    if (!cond) {
        r.ok = false;
        r.errors.push_back(message);
    }
}

CheckResult check_common(const std::string& path) {
    CheckResult r;
    r.label = "common (" + path + ")";
    auto c = config::load_common(path);
    require(r, c.has_value(), c ? "" : c.error().message);
    return r;
}

// Validate one component sub-node of the unified config. `kind` selects the
// component-specific rules; every section additionally bounds postgres_pool_size.
CheckResult check_section(const std::string& kind, const YAML::Node& n) {
    CheckResult r;
    r.label = kind;
    if (!n || !n.IsMap()) {
        require(r, false, "section is missing or not a map");
        return r;
    }

    // instrument_catalog carries Bitget REST config directly; market_history is
    // multi-venue (REST config comes from the per-venue runtime sections).
    if (kind == "instrument_catalog") {
        auto rest = config::parse_bitget_rest(n["bitget"]);
        require(r, rest.has_value(), rest ? "" : rest.error().message);
    }
    if (kind == "market_gateway" || kind == "microstructure_collector") {
        auto ws = config::parse_bitget_ws(n["bitget"]);
        require(r, ws.has_value(), ws ? "" : ws.error().message);
    }
    if (kind == "market_gateway") {
        const int64_t conns = config::opt_int(n, "connections", 2);
        require(r, conns >= 2 && conns <= 8, "connections out of range [2, 8]");
    }
    if (kind == "instrument_catalog") {
        require(r, !config::opt_str(n, "symbols_file", "").empty(), "symbols_file is required");
        const int64_t si = config::opt_int(n, "sync_interval_sec", 900);
        require(r, si >= 60 && si <= 86400, "sync_interval_sec out of range [60, 86400]");
    }
    if (kind == "market_history") {
        const int64_t days = config::opt_int(n, "bootstrap_days", 180);
        require(r, days >= 1 && days <= 365, "bootstrap_days out of range [1, 365]");
        const int64_t rec = config::opt_int(n, "reconcile_interval_sec", 300);
        require(r, rec >= 60 && rec <= 3600, "reconcile_interval_sec out of range [60, 3600]");
        const int64_t overlap = config::opt_int(n, "reconcile_overlap_minutes", 30);
        require(r, overlap >= 5 && overlap <= 120, "reconcile_overlap_minutes out of range [5, 120]");
    }
    if (kind == "storage_writer") {
        const int64_t batch = config::opt_int(n, "batch_size", 2000);
        require(r, batch >= 100 && batch <= 20000, "batch_size out of range [100, 20000]");
    }
    if (kind == "market_gateway") {
        const int64_t qcap = config::opt_int(n, "js_publish_queue_capacity", 8192);
        require(r, qcap >= 256 && qcap <= 1'000'000,
                "js_publish_queue_capacity out of range [256, 1000000]");
    }
    if (kind == "bar_aggregation") {
        const int64_t horizon = config::opt_int(n, "recheck_horizon_min", 35);
        require(r, horizon >= 0 && horizon <= 1440, "recheck_horizon_min out of range [0, 1440]");
    }
    if (kind == "bar_aggregation") {
        const auto tfs = n["timeframes"];
        require(r, tfs && tfs.IsSequence() && tfs.size() > 0, "timeframes list is required");
        if (tfs && tfs.IsSequence()) {
            for (const auto& tf : tfs) {
                const auto label = tf.as<std::string>("");
                require(r, tf_from_label(label).has_value() && label != "1m",
                        "invalid derived timeframe: " + label);
            }
        }
        const int64_t delay = config::opt_int(n, "close_delay_sec", 45);
        require(r, delay >= 5 && delay <= 300, "close_delay_sec out of range [5, 300]");
    }
    if (kind == "data_integrity") {
        const int64_t scan = config::opt_int(n, "gap_scan_interval_sec", 600);
        require(r, scan >= 60 && scan <= 86400, "gap_scan_interval_sec out of range");
        const int64_t attempts = config::opt_int(n, "max_gap_repair_attempts", 5);
        require(r, attempts >= 1 && attempts <= 20, "max_gap_repair_attempts out of range [1, 20]");
        const int64_t deg = config::opt_int(n, "latest_candle_degraded_sec", 300);
        const int64_t fail_s = config::opt_int(n, "latest_candle_failed_sec", 1800);
        require(r, deg >= 120 && fail_s > deg, "health thresholds invalid");
    }

    const int64_t pool = config::opt_int(n, "postgres_pool_size", 2);
    require(r, pool >= 1 && pool <= 16, "postgres_pool_size out of range [1, 16]");
    return r;
}

// Cross-component blocks: exchanges (all futures venues enabled, no spot),
// the storage budget and target>=minimum retention floors.
CheckResult check_multi_venue(const YAML::Node& root) {
    CheckResult r;
    r.label = "four-venue (exchanges/storage/retention)";
    const auto ex = root["exchanges"];
    require(r, ex && ex.IsMap(), "missing 'exchanges' block");
    if (ex && ex.IsMap()) {
        for (const char* v : {"bitget", "binance_usdm", "bybit", "okx"}) {
            require(r, static_cast<bool>(ex[v]), std::string("exchanges.") + v + " missing");
            if (ex[v]) {
                require(r, config::opt_bool(ex[v], "enabled", false),
                        std::string("exchanges.") + v + " must be enabled in production");
            }
        }
        require(r, !ex["binance_spot"] && !ex["spot"],
                "spot markets are not permitted (futures only)");
    }
    const auto uni = root["universe"];
    if (uni && uni.IsMap()) {
        require(r, config::opt_bool(uni, "require_all_exchanges", true),
                "universe.require_all_exchanges must be true (intersection rule)");
    }
    const auto st = root["storage"];
    require(r, st && st.IsMap(), "missing 'storage' block");
    if (st && st.IsMap()) {
        const int64_t hard = config::opt_int(st, "hard_database_limit_bytes", 0);
        require(r, hard >= 1'000'000'000LL && hard <= 2'000'000'000'000LL,
                "storage.hard_database_limit_bytes out of range [1GB, 2TB]");
        const double w = config::opt_double(st, "warning_ratio", 0.8);
        const double c = config::opt_double(st, "cleanup_ratio", 0.87);
        const double cr = config::opt_double(st, "critical_ratio", 0.93);
        require(r, w > 0 && w < c && c < cr && cr < 1.0,
                "storage ratios must satisfy 0 < warning < cleanup < critical < 1");
        // The release margin must not reach the level below, or leaving a state
        // would drop past its neighbour and the ladder would no longer be
        // monotone.
        const double hm = config::opt_double(st, "hysteresis_margin", 0.03);
        const double tightest = std::min({c - w, cr - c, 1.0 - cr});
        require(r, hm >= 0.0 && hm < tightest,
                "storage.hysteresis_margin must be in [0, " + std::to_string(tightest) +
                    ") — smaller than the tightest gap between storage ratios");
    }
    const auto ret = root["retention"];
    if (ret && ret.IsMap()) {
        const auto ge = [&](const char* target, const char* minimum) {
            const int64_t t = config::opt_int(ret, target, 1);
            const int64_t m = config::opt_int(ret, minimum, 0);
            require(r, t >= m,
                    std::string(target) + " must be >= " + minimum);
        };
        ge("candles_1m_days", "candles_1m_min_days");
        ge("candles_agg_days", "candles_agg_min_days");
        ge("derivatives_context_days", "derivatives_context_min_days");
        for (const char* k : {"raw_trades_core_days", "raw_trades_other_hours",
                              "orderbook_core_days", "orderbook_other_hours"}) {
            require(r, config::opt_int(ret, k, 1) > 0,
                    std::string("retention.") + k + " must be positive");
        }

        // A bootstrap deeper than 1m retention downloads history that the next
        // retention pass deletes. Measured on VPS1: bootstrap_days=90 against
        // candles_1m_days=60 dropped 11 partitions mid-backfill and produced
        // 2318 `no partition ... found for row` rejections in two hours, while
        // the venues were being paid for pages that could never survive. The
        // two knobs are one decision and must be validated together.
        // The storage gate must be able to hold what retention is configured to
        // keep. Sizing constants are measured on VPS1 (27 pairs x 4 venues):
        // 155 520 1m rows/day at 225 B, 50 004 aggregate rows/day at 274 B,
        // ~692k core trade rows/hour and ~178k other rows/hour at ~270 B.
        //
        // Getting this wrong is silent and destructive: a 24 GiB limit left
        // over from a 14-day horizon starts shedding at 87% while a 280-day
        // backfill is still running, deleting the history it is loading.
        const auto st_node = root["storage"];
        if (st_node && st_node.IsMap()) {
            const int64_t limit =
                config::opt_int(st_node, "hard_database_limit_bytes", 0);
            const double cleanup = config::opt_double(st_node, "cleanup_ratio", 0.87);
            const int64_t days_1m = config::opt_int(ret, "candles_1m_days", 180);
            const int64_t days_agg = config::opt_int(ret, "candles_agg_days", 365);
            const int64_t hrs_core = config::opt_int(ret, "raw_trades_core_hours", 6);
            const int64_t hrs_other = config::opt_int(ret, "raw_trades_other_hours", 4);

            const double bytes = static_cast<double>(days_1m) * 155'520 * 225 +
                                 static_cast<double>(days_agg) * 50'004 * 274 +
                                 static_cast<double>(hrs_core) * 692'000 * 270 +
                                 static_cast<double>(hrs_other) * 178'000 * 270;
            // Everything not modelled above (1s aggregates, books, 1m series,
            // derivatives, indexes on the small tables) measured at ~4.6 GB.
            const double projected = bytes + 4.6e9;
            const double sheds_at = static_cast<double>(limit) * cleanup;
            require(r, limit > 0 && projected < sheds_at,
                    "storage.hard_database_limit_bytes (" + std::to_string(limit / (1 << 30)) +
                        " GiB) is too small for the configured retention: it projects to ~" +
                        std::to_string(static_cast<int64_t>(projected / 1e9)) +
                        " GB, and aggressive cleanup begins at " +
                        std::to_string(static_cast<int64_t>(sheds_at / 1e9)) +
                        " GB — retention would delete data the backfill is loading");
        }

        const int64_t keep_1m = config::opt_int(ret, "candles_1m_days", 180);
        // All depth-vs-retention rules live in one place, shared with the
        // regression tests, so a rule cannot pass here and be untested there.
        DepthSettings d;
        d.candles_1m_days = config::opt_int(ret, "candles_1m_days", 180);
        d.candles_agg_days = config::opt_int(ret, "candles_agg_days", 365);
        d.derivatives_context_days = config::opt_int(ret, "derivatives_context_days", 180);
        d.raw_trades_core_hours = config::opt_int(ret, "raw_trades_core_hours", 6);
        d.raw_trades_other_hours = config::opt_int(ret, "raw_trades_other_hours", 4);
        if (const auto st_node = root["storage"]; st_node && st_node.IsMap()) {
            d.hard_database_limit_bytes =
                config::opt_int(st_node, "hard_database_limit_bytes", 0);
            d.cleanup_ratio = config::opt_double(st_node, "cleanup_ratio", 0.87);
        }
        if (const auto ops = root["ops_retention"]; ops && ops.IsMap()) {
            d.funding_settlements_days = config::opt_int(ops, "funding_settlements_days", 90);
        }
        if (const auto mh = root["market_history"]; mh && mh.IsMap()) {
            d.bootstrap_days = config::opt_int(mh, "bootstrap_days", 180);
        }

        // Venue depths are per-runtime; the strictest one decides.
        for (const char* venue : {"bitget_context_runtime", "binance_usdm_runtime",
                                  "bybit_runtime", "okx_runtime"}) {
            const auto n = root[venue];
            if (!n || !n.IsMap()) continue;
            d.mark_index_backfill_days = std::max(
                d.mark_index_backfill_days, config::opt_int(n, "mark_index_backfill_days", 0));
            d.funding_backfill_days = std::max(
                d.funding_backfill_days, config::opt_int(n, "funding_backfill_days", 180));
        }

        for (const auto& violation : depth_violations(d)) {
            require(r, false, violation);
        }
    }
    return r;
}

}  // namespace

int cmd_config_check(int argc, char** argv) {
    std::string config_path;
    std::string common_path;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--common" && i + 1 < argc) {
            common_path = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "usage: altspotter config-check --config <altspotter.yaml> "
                         "[--common <common.yaml>]\n";
            return 0;
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            return 2;
        }
    }
    if (config_path.empty()) {
        std::cerr << "usage: altspotter config-check --config <altspotter.yaml> "
                     "[--common <common.yaml>]\n";
        return 2;
    }

    std::vector<CheckResult> results;
    if (!common_path.empty()) results.push_back(check_common(common_path));

    auto doc = config::load_yaml(config_path);
    if (!doc) {
        CheckResult r;
        r.label = "config (" + config_path + ")";
        r.ok = false;
        r.errors.push_back(doc.error().message);
        results.push_back(std::move(r));
    } else {
        static const char* kSections[] = {
            "instrument_catalog", "market_gateway", "microstructure_collector",
            "market_history",     "storage_writer", "bar_aggregation",
            "data_integrity"};
        for (const char* section : kSections) {
            results.push_back(check_section(section, (*doc)[section]));
        }
        results.push_back(check_multi_venue(*doc));
    }

    bool all_ok = true;
    for (const auto& r : results) {
        if (r.ok) {
            std::cout << "OK    " << r.label << "\n";
        } else {
            all_ok = false;
            std::cout << "ERROR " << r.label << "\n";
            for (const auto& e : r.errors) {
                if (!e.empty()) std::cout << "      - " << e << "\n";
            }
        }
    }
    return all_ok ? 0 : 1;
}

}  // namespace altspotter::ops
