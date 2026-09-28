#include "alttech/config/analytics_config.hpp"

#include "alttech/config/config.hpp"
#include "alttech/messaging/topology.hpp"

#include <set>
#include <string>

namespace alttech::config {
namespace {

alttech::Result<YAML::Node> require_map(const YAML::Node& parent, const char* key) {
    const YAML::Node node = parent[key];
    if (!node || !node.IsMap()) {
        return alttech::fail(alttech::ErrorCode::config_error,
                         std::string("missing configuration map: ") + key);
    }
    return node;
}

alttech::Result<void> validate_cycle(const YAML::Node& node, const char* module,
                                 int64_t default_seconds) {
    const int64_t value = alttech::config::opt_int(node, "cycle_interval_sec", default_seconds);
    if (value < 1 || value > 3600) {
        return alttech::fail(alttech::ErrorCode::config_error,
                         std::string(module) + ".cycle_interval_sec out of range [1, 3600]");
    }
    return {};
}

} // namespace

alttech::Result<AnalyticsConfig> parse_analytics_config(const YAML::Node& document) {
    if (!document || !document.IsMap()) {
        return alttech::fail(alttech::ErrorCode::config_error,
                         "analytics configuration must be a YAML map");
    }
    if (alttech::config::opt_str(document, "service", "") != "alttech") {
        return alttech::fail(alttech::ErrorCode::config_error, "service must be 'alttech'");
    }

    const YAML::Node modules = document["modules"];
    if (!modules || !modules.IsMap()) {
        return alttech::fail(alttech::ErrorCode::config_error,
                         "missing configuration map: modules");
    }

    auto feature = require_map(modules, "feature");
    auto microstructure = require_map(modules, "microstructure");
    auto structure = require_map(modules, "structure");
    auto regime = require_map(modules, "regime");
    if (!feature) return std::unexpected(feature.error());
    if (!microstructure) return std::unexpected(microstructure.error());
    if (!structure) return std::unexpected(structure.error());
    if (!regime) return std::unexpected(regime.error());

    const int64_t pool_size = alttech::config::opt_int(document, "postgres_pool_size", 8);
    const int64_t workers = alttech::config::opt_int(*feature, "workers", 1);
    if (pool_size < 5 || pool_size > 64) {
        return alttech::fail(alttech::ErrorCode::config_error,
                         "postgres_pool_size out of range [5, 64]");
    }
    if (workers < 1 || workers > 16) {
        return alttech::fail(alttech::ErrorCode::config_error,
                         "feature.workers out of range [1, 16]");
    }
    const int64_t statement_timeout_ms =
        alttech::config::opt_int(document, "postgres_statement_timeout_ms", 120'000);
    if (statement_timeout_ms != 0 &&
        (statement_timeout_ms < 1'000 || statement_timeout_ms > 600'000)) {
        return alttech::fail(alttech::ErrorCode::config_error,
                         "postgres_statement_timeout_ms must be 0 (disabled) or within [1000, 600000]");
    }
    // Feature workers, its durable consumer, and the other three modules may
    // all hold a database lease concurrently.
    if (pool_size < workers + 4) {
        return alttech::fail(alttech::ErrorCode::config_error,
                         "postgres_pool_size must be at least feature.workers + 4");
    }

    const int64_t chunk = alttech::config::opt_int(*feature, "backfill_chunk", 5000);
    const int64_t batch = alttech::config::opt_int(*feature, "write_batch", 400);
    const int64_t backfill_days = alttech::config::opt_int(*feature, "backfill_days", 2);
    if (chunk < 1 || chunk > 100'000) {
        return alttech::fail(alttech::ErrorCode::config_error,
                         "feature.backfill_chunk out of range [1, 100000]");
    }
    if (batch < 1 || batch > 2000) {
        return alttech::fail(alttech::ErrorCode::config_error,
                         "feature.write_batch out of range [1, 2000]");
    }
    if (backfill_days < 1 || backfill_days > 365) {
        return alttech::fail(alttech::ErrorCode::config_error,
                         "feature.backfill_days out of range [1, 365]");
    }
    const int64_t retry_min =
        alttech::config::opt_int(*feature, "task_retry_backoff_min_ms", 1000);
    const int64_t retry_max =
        alttech::config::opt_int(*feature, "task_retry_backoff_max_ms", 30'000);
    if (retry_min < 100 || retry_min > 60'000 || retry_max < retry_min ||
        retry_max > 300'000) {
        return alttech::fail(alttech::ErrorCode::config_error,
                         "feature retry backoff is outside supported limits");
    }

    const std::string bar_stream = alttech::config::opt_str(
        *feature, "bar_stream",
        std::string(alttech::messaging::topology::kDefaultInputStream));
    const std::string bar_consumer = alttech::config::opt_str(
        *feature, "bar_consumer",
        std::string(alttech::messaging::topology::kDefaultBarConsumer));
    if (!alttech::messaging::topology::valid_resource_name(bar_stream)) {
        return alttech::fail(alttech::ErrorCode::config_error,
                             "feature.bar_stream is not a valid JetStream resource name");
    }
    if (!alttech::messaging::topology::valid_resource_name(bar_consumer)) {
        return alttech::fail(alttech::ErrorCode::config_error,
                             "feature.bar_consumer is not a valid JetStream resource name");
    }

    const std::string timeframes = alttech::config::opt_str(*feature, "timeframes", "");
    const std::set<std::string, std::less<>> allowed = {
        "1m", "5m", "15m", "30m", "1h", "4h", "1d"};
    for (std::size_t begin = 0, end = 0; begin < timeframes.size(); begin = end + 1) {
        end = timeframes.find(',', begin);
        if (end == std::string::npos) end = timeframes.size();
        std::string value = timeframes.substr(begin, end - begin);
        while (!value.empty() && value.front() == ' ') value.erase(value.begin());
        while (!value.empty() && value.back() == ' ') value.pop_back();
        if (!value.empty() && !allowed.contains(value)) {
            return alttech::fail(alttech::ErrorCode::config_error,
                             "unsupported feature timeframe: " + value);
        }
    }

    const std::string persist_windows =
        alttech::config::opt_str(*microstructure, "persist_windows", "15s,30s,1m");
    const std::set<std::string, std::less<>> windows = {"1s", "5s", "15s", "30s", "1m"};
    for (std::size_t begin = 0, end = 0; begin < persist_windows.size(); begin = end + 1) {
        end = persist_windows.find(',', begin);
        if (end == std::string::npos) end = persist_windows.size();
        std::string value = persist_windows.substr(begin, end - begin);
        while (!value.empty() && value.front() == ' ') value.erase(value.begin());
        while (!value.empty() && value.back() == ' ') value.pop_back();
        if (!value.empty() && !windows.contains(value)) {
            return alttech::fail(alttech::ErrorCode::config_error,
                                 "unsupported microstructure.persist_windows entry: " + value);
        }
    }

    const int64_t book_page =
        alttech::config::opt_int(*microstructure, "book_poll_page_size", 50'000);
    const int64_t book_pages =
        alttech::config::opt_int(*microstructure, "book_poll_max_pages_per_tick", 4);
    const int64_t book_overlap =
        alttech::config::opt_int(*microstructure, "book_poll_overlap_ms", 2'000);
    const int64_t store_batch =
        alttech::config::opt_int(*microstructure, "store_batch_size", 2'000);
    const int64_t tick =
        alttech::config::opt_int(*microstructure, "tick_interval_ms", 500);
    if (book_page < 1 || book_page > 50'000 || book_pages < 1 || book_pages > 64 ||
        book_overlap < 0 || book_overlap > 60'000 || store_batch < 1 ||
        store_batch > 10'000 || tick < 100 || tick > 5'000) {
        return alttech::fail(alttech::ErrorCode::config_error,
                         "microstructure polling or batching setting is outside supported limits");
    }

    if (auto valid = validate_cycle(*structure, "structure", 60); !valid) {
        return std::unexpected(valid.error());
    }
    if (auto valid = validate_cycle(*regime, "regime", 30); !valid) {
        return std::unexpected(valid.error());
    }

    // --- venue identity ---
    const std::string primary = alttech::config::opt_str(document, "primary_exchange", "bitget");
    if (primary != "bitget") {
        return alttech::fail(alttech::ErrorCode::config_error,
                             "primary_exchange must be 'bitget'");
    }
    std::vector<std::string> references;
    const YAML::Node refs = document["reference_exchanges"];
    if (refs && refs.IsSequence()) {
        for (const auto& r : refs) references.push_back(r.as<std::string>());
    }
    if (references.empty()) {
        references = {"binance_usdm", "bybit", "okx"};
    }
    const std::set<std::string, std::less<>> allowed_refs = {"binance_usdm", "bybit", "okx"};
    std::set<std::string, std::less<>> seen_refs;
    for (const auto& r : references) {
        if (r == "bitget") {
            return alttech::fail(alttech::ErrorCode::config_error,
                                 "bitget is the primary exchange, not a reference exchange");
        }
        if (r.find("spot") != std::string::npos) {
            return alttech::fail(alttech::ErrorCode::config_error,
                                 "spot markets are out of scope: " + r);
        }
        if (!allowed_refs.contains(r)) {
            return alttech::fail(alttech::ErrorCode::config_error,
                                 "unsupported reference_exchange: " + r);
        }
        seen_refs.insert(r);
    }
    if (seen_refs.size() != 3) {
        return alttech::fail(alttech::ErrorCode::config_error,
                             "reference_exchanges must be exactly binance_usdm, bybit and okx");
    }

    // --- cross-venue module ---
    const YAML::Node cross_venue = document["cross_venue"];
    if (cross_venue && cross_venue.IsMap()) {
        const YAML::Node alignment = cross_venue["alignment"];
        const auto check_ms = [&](const char* key, int64_t def, int64_t lo,
                                  int64_t hi) -> alttech::Result<void> {
            const int64_t v = alttech::config::opt_int(alignment, key, def);
            if (v < lo || v > hi) {
                return alttech::fail(alttech::ErrorCode::config_error,
                                     std::string("cross_venue.alignment.") + key + " out of range");
            }
            return {};
        };
        if (auto v = check_ms("bar_watermark_timeout_ms", 4000, 0, 60000); !v) return std::unexpected(v.error());
        if (auto v = check_ms("max_venue_data_age_ms", 180000, 1000, 3'600'000); !v) {
            return std::unexpected(v.error());
        }
        if (auto v = check_ms("max_cross_venue_skew_ms", 2500, 50, 60000); !v) return std::unexpected(v.error());
        const YAML::Node weighting = cross_venue["weighting"];
        const std::string method = alttech::config::opt_str(weighting, "method", "quality_weighted_median");
        const std::set<std::string, std::less<>> methods = {
            "quality_weighted_median", "quality_weighted_mean", "trimmed_weighted_mean"};
        if (!methods.contains(method)) {
            return alttech::fail(alttech::ErrorCode::config_error,
                                 "cross_venue.weighting.method is not a supported estimator");
        }
    }

    // --- storage budget ---
    const YAML::Node storage = document["storage"];
    const int64_t hard_limit =
        alttech::config::opt_int(storage, "total_database_hard_limit_bytes", 20'000'000'000);
    const int64_t soft_limit =
        alttech::config::opt_int(storage, "analytics_soft_limit_bytes", 3'000'000'000);
    const double warn = alttech::config::opt_double(storage, "analytics_warning_ratio", 0.80);
    const double cleanup = alttech::config::opt_double(storage, "analytics_cleanup_ratio", 0.90);
    const double critical = alttech::config::opt_double(storage, "analytics_critical_ratio", 0.95);
    if (hard_limit <= 0 || soft_limit <= 0 || soft_limit >= hard_limit) {
        return alttech::fail(alttech::ErrorCode::config_error,
                             "storage: analytics_soft_limit must be > 0 and < the hard limit");
    }
    if (!(warn > 0.0 && warn < cleanup && cleanup < critical && critical < 1.0)) {
        return alttech::fail(alttech::ErrorCode::config_error,
                             "storage ratios must satisfy 0 < warning < cleanup < critical < 1");
    }

    AnalyticsConfig result;
    result.postgres_pool_size = static_cast<std::size_t>(pool_size);
    result.postgres_statement_timeout_ms = statement_timeout_ms;
    result.feature = *feature;
    result.microstructure = *microstructure;
    result.structure = *structure;
    result.regime = *regime;
    result.cross_venue = cross_venue ? cross_venue : YAML::Node(YAML::NodeType::Map);
    const YAML::Node macro = document["macro"];
    result.macro = macro ? macro : YAML::Node(YAML::NodeType::Map);
    result.primary_exchange = primary;
    result.reference_exchanges = references;
    result.total_database_hard_limit_bytes = hard_limit;
    result.analytics_soft_limit_bytes = soft_limit;
    result.analytics_warning_ratio = warn;
    result.analytics_cleanup_ratio = cleanup;
    result.analytics_critical_ratio = critical;
    return result;
}

} // namespace alttech::config
