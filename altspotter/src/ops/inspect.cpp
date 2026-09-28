// altspotter inspect: operational report over stored market history.
//
//   altspotter inspect [--dsn DSN] [--symbol BTCUSDT]
//   DSN fallback: ALTSPOTTER_POSTGRES_DSN.
//
// Prints per-instrument coverage (earliest/latest 1m, rows, gaps, status),
// aggregate presence per timeframe and duplicate-key sanity checks.

#include "ops/ops.hpp"
#include <pqxx/pqxx>

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>

namespace altspotter::ops {

namespace {

int inspect_universe(pqxx::read_transaction& txn) {
    std::cout << "=== global observation universe ===\n";
    std::cout << std::left << std::setw(10) << "base" << std::setw(24) << "status"
              << std::setw(8) << "elig" << std::setw(6) << "bg" << std::setw(6) << "bn"
              << std::setw(6) << "ok" << std::setw(6) << "by" << "reason\n";
    const auto rows = txn.exec(
        "SELECT cp.base_asset, u.status, u.eligible, u.bitget_active, u.binance_active, "
        "u.okx_active, u.bybit_active, COALESCE(u.reason,'') "
        "FROM global_observation_universe u JOIN canonical_pairs cp ON cp.id = u.canonical_pair_id "
        "ORDER BY u.eligible DESC, cp.base_asset");
    int eligible = 0;
    for (const auto& r : rows) {
        if (r[2].as<bool>()) ++eligible;
        std::cout << std::left << std::setw(10) << r[0].as<std::string>("?") << std::setw(24)
                  << r[1].as<std::string>("?") << std::setw(8) << (r[2].as<bool>() ? "yes" : "no")
                  << std::setw(6) << (r[3].as<bool>() ? "Y" : "-") << std::setw(6)
                  << (r[4].as<bool>() ? "Y" : "-") << std::setw(6) << (r[5].as<bool>() ? "Y" : "-")
                  << std::setw(6) << (r[6].as<bool>() ? "Y" : "-") << r[7].as<std::string>("")
                  << "\n";
    }
    std::cout << "\neligible pairs: " << eligible << " / " << rows.size() << "\n";
    return 0;
}

int inspect_exchanges(pqxx::read_transaction& txn) {
    std::cout << "=== venue connections ===\n";
    const auto rows = txn.exec(
        "SELECT e.code, ec.connection_id, ec.state, ec.reconnect_count, "
        "to_char(ec.updated_at, 'YYYY-MM-DD HH24:MI:SS') "
        "FROM exchange_connections ec JOIN exchanges e ON e.id = ec.exchange_id "
        "ORDER BY e.code");
    std::cout << std::left << std::setw(16) << "venue" << std::setw(24) << "connection"
              << std::setw(14) << "state" << std::setw(10) << "reconn" << "updated\n";
    for (const auto& r : rows) {
        std::cout << std::left << std::setw(16) << r[0].as<std::string>("?") << std::setw(24)
                  << r[1].as<std::string>("?") << std::setw(14) << r[2].as<std::string>("?")
                  << std::setw(10) << r[3].as<std::string>("0") << r[4].as<std::string>("-")
                  << "\n";
    }
    std::cout << "\n=== active symbols per venue ===\n";
    const auto counts = txn.exec(
        "SELECT e.code, count(*) FILTER (WHERE ei.is_whitelisted AND ei.status='active') "
        "FROM exchanges e LEFT JOIN exchange_instruments ei ON ei.exchange_id = e.id "
        "GROUP BY e.code ORDER BY e.code");
    for (const auto& r : counts) {
        std::cout << std::left << std::setw(16) << r[0].as<std::string>("?")
                  << r[1].as<std::string>("0") << " active\n";
    }
    return 0;
}

int inspect_storage(pqxx::read_transaction& txn) {
    std::cout << "=== storage budget (latest snapshot) ===\n";
    const auto snap = txn.exec(
        "SELECT db_bytes, hard_limit_bytes, state, persistence_mode, "
        "to_char(captured_at,'YYYY-MM-DD HH24:MI:SS') "
        "FROM storage_budget_snapshots ORDER BY captured_at DESC LIMIT 1");
    if (!snap.empty()) {
        const int64_t db = snap[0][0].as<int64_t>();
        const int64_t hard = snap[0][1].as<int64_t>();
        std::cout << "db size     : " << db / (1024 * 1024) << " MiB\n"
                  << "hard limit  : " << hard / (1024 * 1024) << " MiB\n"
                  << "usage       : " << (hard > 0 ? db * 100 / hard : 0) << "%\n"
                  << "state       : " << snap[0][2].as<std::string>("?") << "\n"
                  << "persistence : " << snap[0][3].as<std::string>("?") << "\n"
                  << "captured    : " << snap[0][4].as<std::string>("-") << "\n";
    } else {
        std::cout << "(no snapshot yet)\n";
    }
    std::cout << "\n=== largest tables (whole partition tree) ===\n";
    // Sum every leaf partition — pg_total_relation_size on a partitioned parent
    // reports only the empty parent.
    const auto tables = txn.exec(
        "SELECT c.relname, pg_size_pretty(COALESCE((SELECT sum(pg_total_relation_size(t.relid)) "
        "  FROM pg_partition_tree(c.oid) t WHERE t.isleaf), 0)) AS sz, "
        "  COALESCE((SELECT sum(pg_total_relation_size(t.relid)) "
        "  FROM pg_partition_tree(c.oid) t WHERE t.isleaf), 0) AS bytes "
        "FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace "
        "WHERE n.nspname='public' AND c.relkind='p' AND c.relispartition=false "
        "  AND c.relname LIKE 'market\\_%' "
        "ORDER BY bytes DESC LIMIT 20");
    for (const auto& r : tables) {
        std::cout << std::left << std::setw(36) << r[0].as<std::string>("?")
                  << r[1].as<std::string>("-") << "\n";
    }
    return 0;
}

int inspect_retention(pqxx::read_transaction& txn) {
    std::cout << "=== retention policies ===\n";
    const auto pol = txn.exec(
        "SELECT table_name, retention_class, target_keep::text, minimum_keep::text, "
        "cleanup_priority FROM retention_policies ORDER BY cleanup_priority");
    std::cout << std::left << std::setw(34) << "table" << std::setw(8) << "class"
              << std::setw(14) << "target" << std::setw(14) << "minimum" << "prio\n";
    for (const auto& r : pol) {
        std::cout << std::left << std::setw(34) << r[0].as<std::string>("?") << std::setw(8)
                  << r[1].as<std::string>("?") << std::setw(14) << r[2].as<std::string>("?")
                  << std::setw(14) << r[3].as<std::string>("?") << r[4].as<std::string>("?")
                  << "\n";
    }
    std::cout << "\n=== recent retention runs ===\n";
    const auto runs = txn.exec(
        "SELECT to_char(started_at,'MM-DD HH24:MI'), mode, budget_state, partitions_dropped, "
        "COALESCE((db_bytes_before-db_bytes_after)/1024/1024, 0) "
        "FROM retention_runs ORDER BY started_at DESC LIMIT 10");
    for (const auto& r : runs) {
        std::cout << r[0].as<std::string>("-") << "  " << r[1].as<std::string>("?") << "  "
                  << r[2].as<std::string>("?") << "  dropped=" << r[3].as<std::string>("0")
                  << "  freed=" << r[4].as<std::string>("0") << "MiB\n";
    }
    return 0;
}

int inspect_quality(pqxx::read_transaction& txn) {
    std::cout << "=== data quality ===\n";
    const auto rows = txn.exec(
        "SELECT e.code, ei.exchange_symbol, dq.quality, dq.trade_age_sec, dq.book_age_sec, "
        "dq.sequence_gap_count, dq.crossed_book_count "
        "FROM exchange_data_quality dq JOIN exchanges e ON e.id = dq.exchange_id "
        "JOIN exchange_instruments ei ON ei.exchange_id=dq.exchange_id "
        "  AND ei.instrument_id=dq.instrument_id "
        "ORDER BY e.code, ei.exchange_symbol LIMIT 200");
    std::cout << std::left << std::setw(14) << "venue" << std::setw(14) << "symbol"
              << std::setw(12) << "quality" << std::setw(10) << "trade_s" << std::setw(10)
              << "book_s" << std::setw(8) << "gaps" << "crossed\n";
    for (const auto& r : rows) {
        std::cout << std::left << std::setw(14) << r[0].as<std::string>("?") << std::setw(14)
                  << r[1].as<std::string>("?") << std::setw(12) << r[2].as<std::string>("?")
                  << std::setw(10) << r[3].as<std::string>("-") << std::setw(10)
                  << r[4].as<std::string>("-") << std::setw(8) << r[5].as<std::string>("0")
                  << r[6].as<std::string>("0") << "\n";
    }
    return 0;
}

} // namespace

int cmd_inspect(int argc, char** argv) {
    std::string dsn;
    std::string symbol;
    std::string mode = "coverage";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--dsn" && i + 1 < argc) {
            dsn = argv[++i];
        } else if (arg == "--symbol" && i + 1 < argc) {
            symbol = argv[++i];
        } else if (arg == "--universe") {
            mode = "universe";
        } else if (arg == "--exchanges") {
            mode = "exchanges";
        } else if (arg == "--storage") {
            mode = "storage";
        } else if (arg == "--retention") {
            mode = "retention";
        } else if (arg == "--quality") {
            mode = "quality";
        } else if (arg == "--growth") {
            mode = "storage";
        } else if (arg == "--coverage") {
            mode = "coverage";
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "usage: altspotter inspect [--dsn DSN] [--symbol SYMBOL] "
                         "[--coverage|--universe|--exchanges|--storage|--retention|--quality]\n";
            return 0;
        }
    }
    if (dsn.empty()) {
        if (const char* env = std::getenv("ALTSPOTTER_POSTGRES_DSN")) dsn = env;
    }
    if (dsn.empty()) {
        std::cerr << "no DSN: use --dsn or ALTSPOTTER_POSTGRES_DSN\n";
        return 2;
    }

    try {
        pqxx::connection conn(dsn);
        pqxx::read_transaction txn(conn);
        txn.exec("SET TIME ZONE 'UTC'");

        if (mode == "universe") return inspect_universe(txn);
        if (mode == "exchanges") return inspect_exchanges(txn);
        if (mode == "storage") return inspect_storage(txn);
        if (mode == "retention") return inspect_retention(txn);
        if (mode == "quality") return inspect_quality(txn);

        std::string filter;
        if (!symbol.empty()) filter = " AND ei.exchange_symbol = " + txn.quote(symbol);

        std::cout << "=== 1m coverage (exchange=bitget) ===\n";
        std::cout << std::left << std::setw(12) << "symbol" << std::setw(10) << "status"
                  << std::setw(22) << "earliest_1m" << std::setw(22) << "latest_1m"
                  << std::setw(10) << "rows" << std::setw(6) << "gaps" << std::setw(6) << "open"
                  << "cov_status\n";

        const auto rows = txn.exec(
            "SELECT ei.exchange_symbol, ei.status, "
            "       to_char(hc.actual_from, 'YYYY-MM-DD HH24:MI'), "
            "       to_char(hc.covered_until, 'YYYY-MM-DD HH24:MI'), "
            "       hc.row_count, hc.gap_count, hc.open_gap_count, hc.status "
            "FROM exchange_instruments ei "
            "JOIN exchanges e ON e.id = ei.exchange_id AND e.code = 'bitget' "
            "LEFT JOIN history_coverage hc ON hc.exchange_id = ei.exchange_id "
            "  AND hc.instrument_id = ei.instrument_id AND hc.timeframe = '1m' "
            "WHERE ei.is_whitelisted" + filter +
            " ORDER BY ei.exchange_symbol");
        for (const auto& r : rows) {
            std::cout << std::left << std::setw(12) << r[0].as<std::string>("?") << std::setw(10)
                      << r[1].as<std::string>("?") << std::setw(22) << r[2].as<std::string>("-")
                      << std::setw(22) << r[3].as<std::string>("-") << std::setw(10)
                      << r[4].as<std::string>("0") << std::setw(6) << r[5].as<std::string>("0")
                      << std::setw(6) << r[6].as<std::string>("0") << r[7].as<std::string>("-")
                      << "\n";
        }

        std::cout << "\n=== aggregated bars per timeframe ===\n";
        const auto agg = txn.exec(
            "SELECT timeframe, count(*), min(open_time)::date, max(open_time), "
            "       count(*) FILTER (WHERE quality_status = 'incomplete') "
            "FROM market_candles_agg GROUP BY timeframe "
            "ORDER BY CASE timeframe WHEN '5m' THEN 1 WHEN '15m' THEN 2 WHEN '30m' THEN 3 "
            "WHEN '1h' THEN 4 WHEN '4h' THEN 5 WHEN '1d' THEN 6 END");
        std::cout << std::left << std::setw(6) << "tf" << std::setw(12) << "bars" << std::setw(14)
                  << "from" << std::setw(24) << "latest" << "incomplete\n";
        for (const auto& r : agg) {
            std::cout << std::left << std::setw(6) << r[0].as<std::string>("?") << std::setw(12)
                      << r[1].as<std::string>("0") << std::setw(14) << r[2].as<std::string>("-")
                      << std::setw(24) << r[3].as<std::string>("-") << r[4].as<std::string>("0")
                      << "\n";
        }

        std::cout << "\n=== sanity ===\n";
        const auto dup1 = txn.exec1(
            "SELECT count(*) FROM (SELECT exchange_id, instrument_id, open_time "
            "FROM market_candles_1m GROUP BY 1, 2, 3 HAVING count(*) > 1) d");
        std::cout << "duplicate 1m natural keys: " << dup1[0].as<int64_t>() << "\n";
        const auto total = txn.exec1("SELECT count(*) FROM market_candles_1m");
        std::cout << "total 1m rows: " << total[0].as<int64_t>() << "\n";
        const auto open_gaps = txn.exec1(
            "SELECT count(*) FROM history_gaps WHERE status IN ('open', 'repairing')");
        std::cout << "unresolved gaps: " << open_gaps[0].as<int64_t>() << "\n";
        const auto rejected = txn.exec1("SELECT count(*) FROM rejected_market_events");
        std::cout << "rejected events: " << rejected[0].as<int64_t>() << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "inspect failed: " << e.what() << "\n";
        return 1;
    }
}

}  // namespace altspotter::ops
