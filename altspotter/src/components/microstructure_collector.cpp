// microstructure_collector
//
// Standalone collector of the two data sources price-derived indicators cannot
// reconstruct: the raw taker trade stream and the resting order book. It opens
// its OWN WebSocket connection(s) to Bitget public depth + trade channels and
// writes directly to market_trades_raw and market_orderbook_snapshots.
//
// Deliberately independent of market_gateway: a fault here can never affect the
// candle/trade ingestion the rest of the system relies on. Every row is
// idempotent (natural PK).
//
// Book snapshots use the SAME sampling discipline as the other venue runtimes
// (`sampling.orderbook_core_interval_ms` / `_other_interval_ms`, keyed on the
// instrument's retention class). Sampling one venue more densely than the
// others would make the 1s/1m book aggregates statistically incomparable across
// venues — which is exactly what the four-venue universe exists to compare.

#include "components/components.hpp"
#include "core/persistence_gate.hpp"
#include "core/readiness.hpp"
#include "core/shutdown.hpp"
#include "config/config.hpp"
#include "exchanges/bitget/bitget_ws.hpp"
#include "exchanges/adapter_util.hpp"
#include "exchanges/market_data_adapter.hpp"
#include "logging/logger.hpp"
#include "persistence/pg.hpp"
#include "core/time.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

using namespace altspotter;
using json = nlohmann::json;

namespace {

constexpr int kExchangeId = 1; // bitget


// One observed Bitget instrument: internal id + retention class (1 core, 0 other).
struct Instrument {
    int64_t instrument_id = 0;
    bool core = false;

    bool operator==(const Instrument&) const = default;
};

Result<std::map<std::string, Instrument>> load_whitelisted(persistence::PgPool& pool) {
    std::map<std::string, Instrument> out;
    auto res = pool.with_txn([&](pqxx::work& txn) {
        const auto rows = txn.exec(
            "SELECT ei.exchange_symbol, ei.instrument_id, (rca.base_asset IS NOT NULL) "
            "FROM exchange_instruments ei "
            "JOIN exchanges e ON e.id = ei.exchange_id "
            "JOIN instruments i ON i.id = ei.instrument_id "
            "LEFT JOIN retention_core_assets rca ON rca.base_asset = i.base_asset "
            "WHERE e.code = 'bitget' AND ei.is_whitelisted AND ei.status = 'active' "
            "ORDER BY ei.exchange_symbol");
        for (const auto& r : rows) {
            out.emplace(r[0].as<std::string>(),
                        Instrument{r[1].as<int64_t>(), r[2].as<bool>()});
        }
    });
    if (!res) return std::unexpected(res.error());
    return out;
}

std::string levels_json(const std::vector<exchanges::OrderBookLevel>& lv) {
    json a = json::array();
    for (const auto& l : lv) a.push_back({l.price, l.size});
    return a.dump();
}

class Collector {
public:
    Collector(config::ServiceContext ctx, config::BitgetWsConfig ws_cfg,
              std::map<std::string, Instrument> instruments)
        : ctx_(std::move(ctx)),
          pool_(ctx_.common.postgres_dsn,
                static_cast<size_t>(config::opt_int(ctx_.service, "postgres_pool_size", 2))),
          instruments_(std::move(instruments)),
          reload_interval_sec_(config::opt_int(ctx_.service, "instrument_reload_sec", 300)),
          stream_(std::make_unique<exchanges::bitget::BitgetWsManager>(std::move(ws_cfg), 1)) {
        const auto sampling = ctx_.root["sampling"];
        core_interval_ms_ = config::opt_int(sampling, "orderbook_core_interval_ms", 5000);
        other_interval_ms_ = config::opt_int(sampling, "orderbook_other_interval_ms", 15000);
    }

    void run() {
        exchanges::StreamCallbacks cb;
        cb.on_trade = [this](const std::string& sym, const Trade& t, int64_t, const std::string&) {
            write_trade(sym, t);
        };
        cb.on_orderbook = [this](const std::string& sym, const exchanges::OrderBook& b, int64_t,
                                 const std::string&) { write_book(sym, b); };
        cb.on_connection_event = [](const std::string& kind, const std::string& conn,
                                    const std::string& detail, int32_t subs) {
            log::info("collector_connection", {{"kind", kind}, {"connection_id", conn},
                                               {"detail", detail}, {"subscribed", subs}});
        };
        auto& shutdown = ShutdownToken::instance();
        stream_->start(symbols(), std::move(cb));
        log::info("collector_started",
                  {{"instruments", static_cast<int>(symbols().size())},
                   {"orderbook_core_interval_ms", core_interval_ms_},
                   {"orderbook_other_interval_ms", other_interval_ms_}});
        // Follow the catalog: newly enabled instruments must be collected
        // without a process restart, disabled ones must be unsubscribed.
        int64_t last_reload = timeutil::now_ms();
        while (!shutdown.wait_for(std::chrono::seconds(1))) {
            if (timeutil::now_ms() - last_reload < reload_interval_sec_ * 1000) continue;
            last_reload = timeutil::now_ms();
            auto fresh = load_whitelisted(pool_);
            if (!fresh) {
                log::warn("collector_reload_failed", {{"message", fresh.error().message}});
                continue;
            }
            bool changed = false;
            {
                std::lock_guard<std::mutex> lk(mu_);
                changed = *fresh != instruments_;
                if (changed) instruments_ = std::move(*fresh);
            }
            if (changed) {
                const auto syms = symbols();
                log::info("collector_instruments_changed", {{"count", syms.size()}});
                stream_->update_symbols(syms);
            }
        }
        stream_->stop();
        log::info("collector_stopped",
                  {{"trades", trades_.load()}, {"books", books_.load()},
                   {"books_shed", books_shed_.load()}});
    }

private:
    std::vector<std::string> symbols() {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<std::string> out;
        out.reserve(instruments_.size());
        for (const auto& [s, _] : instruments_) out.push_back(s);
        return out;
    }

    std::optional<Instrument> lookup(const std::string& sym) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = instruments_.find(sym);
        if (it == instruments_.end()) return std::nullopt;
        return it->second;
    }

    void write_trade(const std::string& sym, const Trade& t) {
        const auto instr = lookup(sym);
        if (!instr || t.exchange_ts_ms <= 0 || t.exchange_trade_id.empty()) return;
        if (!PersistenceGate::instance().allow_raw_trades()) return;
        const char* side = t.side == TradeSide::buy ? "buy"
                           : t.side == TradeSide::sell ? "sell" : nullptr;
        if (!side) return;
        auto res = pool_.with_txn([&](pqxx::work& txn) {
            txn.exec_params(
                "INSERT INTO market_trades_raw "
                "(exchange_id, instrument_id, trade_time, trade_id, price, size, side, "
                " retention_class) "
                "VALUES ($1,$2,to_timestamp($3::double precision/1000.0),$4,$5,$6,$7,$8) "
                "ON CONFLICT DO NOTHING",
                kExchangeId, instr->instrument_id, t.exchange_ts_ms, t.exchange_trade_id,
                t.price.to_string(), t.size.to_string(), side, instr->core ? 1 : 0);
        });
        if (res) trades_.fetch_add(1);
        else log::warn("trade_write_failed", {{"symbol", sym}, {"message", res.error().message}});
    }

    void write_book(const std::string& sym, const exchanges::OrderBook& b) {
        const auto instr = lookup(sym);
        if (!instr || b.bids.empty() || b.asks.empty()) return;
        if (!PersistenceGate::instance().allow_book_snapshots()) {
            // Count it. This return is where the order book was lost for the
            // entire life of the deployment: the storage budget held the gate
            // in DEGRADED_BOOK_STORAGE_DISABLED, raw trades were still allowed
            // through the neighbouring predicate, and the asymmetry was
            // invisible because shedding a book wrote nothing anywhere. A
            // silent drop of real market data is not an acceptable steady
            // state, so it is now counted and named with the mode that caused
            // it, throttled to one line a minute.
            const int64_t shed = books_shed_.fetch_add(1) + 1;
            const int64_t now = timeutil::now_ms();
            int64_t last = last_shed_log_ms_.load();
            if (now - last >= 60'000 && last_shed_log_ms_.compare_exchange_strong(last, now)) {
                log::warn("book_snapshots_shed",
                          {{"total_shed", shed},
                           {"persistence_mode",
                            std::string(persistence_mode_label(PersistenceGate::instance().mode()))},
                           {"reason", "storage budget: raise hard_database_limit_bytes or "
                                      "shorten retention; see docs/STORAGE_BUDGET.md"}});
            }
            return;
        }
        // Sample at the venue-agnostic cadence for this instrument's class.
        const int64_t now = timeutil::now_ms();
        const int64_t interval = instr->core ? core_interval_ms_ : other_interval_ms_;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto& last = last_book_ms_[sym];
            if (now - last < interval) return;
            last = now;
        }
        using exchanges::to_double;
        const double bid = to_double(b.bids.front().price);
        const double ask = to_double(b.asks.front().price);
        const double bsz = to_double(b.bids.front().size);
        const double asz = to_double(b.asks.front().size);
        // to_double() has already mapped non-finite input to 0, so `> 0` is now
        // a real barrier rather than one a NaN slips past.
        if (!(bid > 0) || !(ask > 0) || !(ask >= bid)) return;
        const double mid = (bid + ask) / 2.0;
        const double micro = (bsz + asz) > 0 ? (bid * asz + ask * bsz) / (bsz + asz) : mid;
        const double spread_bps = mid > 0 ? (ask - bid) / mid * 1e4 : 0.0;
        double bd = 0, ad = 0;
        for (const auto& l : b.bids) bd += to_double(l.size);
        for (const auto& l : b.asks) ad += to_double(l.size);
        const double imb = (bd + ad) > 0 ? (bd - ad) / (bd + ad) : 0.0;
        const std::string bids_j = levels_json(b.bids), asks_j = levels_json(b.asks);
        const int levels = static_cast<int>(std::max(b.bids.size(), b.asks.size()));
        auto res = pool_.with_txn([&](pqxx::work& txn) {
            txn.exec_params(
                "INSERT INTO market_orderbook_snapshots "
                "(exchange_id, instrument_id, snapshot_time, seq, best_bid, best_ask, mid, "
                " microprice, spread_bps, bid_depth_n, ask_depth_n, imbalance_n, levels, bids, "
                " asks, retention_class) "
                "VALUES ($1,$2,to_timestamp($3::double precision/1000.0),$4,$5,$6,$7,$8,$9,$10,$11,"
                "$12,$13,$14::jsonb,$15::jsonb,$16) ON CONFLICT DO NOTHING",
                kExchangeId, instr->instrument_id, b.exchange_ms > 0 ? b.exchange_ms : now, b.seq,
                bid, ask, mid, micro, spread_bps, bd, ad, imb, levels, bids_j, asks_j,
                instr->core ? 1 : 0);
        });
        if (res) books_.fetch_add(1);
        else log::warn("book_write_failed", {{"symbol", sym}, {"message", res.error().message}});
    }

    config::ServiceContext ctx_;
    persistence::PgPool pool_;
    std::map<std::string, Instrument> instruments_; // guarded by mu_
    int64_t core_interval_ms_ = 5000;
    int64_t other_interval_ms_ = 15000;
    int64_t reload_interval_sec_;
    std::unique_ptr<exchanges::bitget::BitgetWsManager> stream_;
    std::mutex mu_; // guards instruments_ and last_book_ms_
    std::map<std::string, int64_t> last_book_ms_;
    std::atomic<int64_t> trades_{0}, books_{0};
    // Book snapshots received from the venue and then dropped before storage.
    std::atomic<int64_t> books_shed_{0};
    std::atomic<int64_t> last_shed_log_ms_{0};
};

} // namespace

namespace altspotter::components {

int microstructure_collector(config::ServiceContext ctx) {
    auto ws_cfg = config::parse_bitget_ws(ctx.service["bitget"]);
    if (!ws_cfg) {
        log::critical("bad_config", {{"message", ws_cfg.error().message}});
        return 2;
    }
    persistence::PgPool probe_pool(ctx.common.postgres_dsn, 1);
    auto instruments = load_whitelisted(probe_pool);
    if (!instruments) {
        log::critical("instrument_load_failed", {{"message", instruments.error().message}});
        return 1;
    }
    if (instruments->empty()) {
        // First boot: the catalog populates instruments shortly after start.
        // Stay up and pick them up via the periodic reload instead of cycling
        // through supervisor restarts.
        log::warn("no_whitelisted_instruments",
                  {{"message", "catalog has no active instruments yet; will follow reloads"}});
    }

    Collector collector(std::move(ctx), std::move(*ws_cfg), std::move(*instruments));
    collector.run();
    return 0;
}

}  // namespace altspotter::components
