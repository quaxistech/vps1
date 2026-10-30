// exchange_runtime: one supervised runtime per futures venue (binance_usdm,
// bybit, okx) plus a context-only runtime for bitget (whose candles/trades/
// books stay on the proven dedicated components).
//
// Per venue this component owns:
//   * WS session(s) via the generic VenueWsSession engine (OKX: public +
//     business endpoints; Binance: market + public stream groups), sharded
//     state per instrument, no global locks;
//   * local order books with the venue's official snapshot/delta sequencing
//     (desync -> REST snapshot or resubscribe -> recovered, integrity events);
//   * publishing: trades / book deltas+snapshots / candle updates on Core
//     NATS; closed 1m bars, BBO/ticker/mark/index/funding/OI (throttled) and
//     gateway/integrity events on JetStream via a bounded publish queue;
//   * persistence: batched raw trades + sampled top-15 book snapshots
//     (retention-class aware, gated by the storage budget), per-minute
//     derivatives context and BBO rows;
//   * REST helpers: OI polling and funding-settlement backfill, plus Binance
//     depth snapshots for book resync;
//   * operational state: exchange_connections / exchange_data_quality /
//     exchange_clock_offsets.
//
// The observed instrument set is the venue slice of the global observation
// universe (exchange_instruments.is_whitelisted AND status='active'), refreshed
// periodically; a globally excluded pair therefore has no subscriptions here.

#include "components/components.hpp"
#include "components/funding_sync.hpp"
#include "core/persistence_gate.hpp"
#include "core/readiness.hpp"
#include "core/shutdown.hpp"
#include "config/config.hpp"
#include "exchanges/binance/binance_adapter.hpp"
#include "exchanges/bitget/bitget_adapter.hpp"
#include "exchanges/bitget/bitget_context.hpp"
#include "exchanges/bybit/bybit_adapter.hpp"
#include "exchanges/okx/okx_adapter.hpp"
#include "exchanges/order_book.hpp"
#include "exchanges/venue_stream.hpp"
#include "exchanges/venue_ws.hpp"
#include "exchanges/venues.hpp"
#include "logging/logger.hpp"
#include "messaging/codec.hpp"
#include "messaging/js_queue.hpp"
#include "messaging/nats_client.hpp"
#include "messaging/subjects.hpp"
#include "persistence/pg.hpp"
#include "persistence/txn_log.hpp"
#include "core/time.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <thread>

using namespace altspotter;
using altspotter::exchanges::BookApplyResult;
using altspotter::exchanges::BookUpdate;
using altspotter::exchanges::Level;

namespace {

struct RuntimeInstrument {
    int64_t instrument_id = 0;
    std::string symbol;
    std::string base_asset;
    bool core = false;
    Decimal contract_value{1, 0};
};

struct PendingTrade {
    int64_t instrument_id;
    bool core;
    Trade trade;
};

struct PendingSnapshot {
    int64_t instrument_id;
    bool core;
    int64_t snapshot_time_ms;
    int64_t seq;
    std::vector<Level> bids;
    std::vector<Level> asks;
};

struct BboMinute {
    std::optional<Decimal> bid_first, bid_last, bid_min, bid_max;
    std::optional<Decimal> ask_first, ask_last, ask_min, ask_max;
    std::optional<Decimal> spread_min, spread_max;
    int samples = 0;
};

// Last-value merge of the venue's context channels + freshness bookkeeping.
struct ContextState {
    std::optional<Decimal> mark, index, funding, oi_base, oi_quote, turnover_24h;
    std::optional<Decimal> best_bid, best_ask;
    int64_t next_funding_ms = 0;
    uint32_t mask = 0; // 1 mark,2 index,4 funding,8 oi,16 bbo,32 ticker
    BboMinute bbo;
    // publish throttles (ms of last JetStream publish per kind)
    int64_t pub_ticker = 0, pub_mark = 0, pub_index = 0, pub_funding = 0, pub_oi = 0,
            pub_bbo = 0;
    // freshness (exchange event ms of last seen)
    int64_t seen_trade = 0, seen_candle = 0, seen_book = 0, seen_ticker = 0, seen_mark = 0,
            seen_oi = 0;
    int64_t seq_gaps = 0, crossed = 0;
};

struct BookState {
    std::mutex mu;
    std::unique_ptr<exchanges::LocalBook> book;
    bool desynced = false;
    int64_t last_snapshot_row_ms = 0;
    int64_t last_resync_ms = 0;
};

struct VenueSpec {
    int16_t exchange_id;
    const char* code;
    const char* service_name;
    bool context_only; // bitget: ticker context only
    exchanges::LocalBook::Sequencing sequencing;
};

class VenueRuntime {
public:
    VenueRuntime(config::ServiceContext ctx, VenueSpec spec)
        : ctx_(std::move(ctx)), spec_(spec),
          pool_(ctx_.common.postgres_dsn,
                static_cast<size_t>(config::opt_int(ctx_.service, "postgres_pool_size", 2))) {
        core_interval_ms_ = config::opt_int(ctx_.service["sampling"], "orderbook_core_interval_ms",
                                            5000);
        other_interval_ms_ =
            config::opt_int(ctx_.service["sampling"], "orderbook_other_interval_ms", 15000);
        snapshot_depth_ =
            static_cast<int>(config::opt_int(ctx_.service["sampling"], "orderbook_depth", 15));
        bbo_pub_interval_ms_ = config::opt_int(ctx_.service, "bbo_publish_interval_ms", 1000);
        ticker_pub_interval_ms_ = config::opt_int(ctx_.service, "ticker_publish_interval_ms", 1000);
        context_pub_interval_ms_ =
            config::opt_int(ctx_.service, "context_publish_interval_ms", 3000);
        flush_interval_ms_ = config::opt_int(ctx_.service, "flush_interval_ms", 1000);
        reload_interval_sec_ = config::opt_int(ctx_.service, "instrument_reload_sec", 120);
        oi_poll_interval_sec_ = config::opt_int(ctx_.service, "oi_poll_interval_sec", 60);
        funding_backfill_days_ = config::opt_int(ctx_.service, "funding_backfill_days", 180);
        mark_index_backfill_days_ =
            config::opt_int(ctx_.service, "mark_index_backfill_days", 0);
        max_pending_rows_ = static_cast<size_t>(
            config::opt_int(ctx_.service, "max_pending_rows", 200000));
    }

    int run() {
        auto nats = messaging::NatsClient::connect(ctx_.common.nats_url, spec_.service_name);
        if (!nats) {
            log::critical("nats_unavailable", {{"message", nats.error().message}});
            return 1;
        }
        nats_ = std::move(*nats);
        js_ = std::make_unique<messaging::JsPublishQueue>(
            *nats_, static_cast<size_t>(config::opt_int(ctx_.service, "js_queue_capacity", 8192)),
            std::string(spec_.code) + "-context");

        if (auto probe = pool_.with_txn([](pqxx::work& txn) { txn.exec1("SELECT 1"); }); !probe) {
            log::critical("postgres_unavailable", {{"message", probe.error().message}});
            return 1;
        }
        make_adapter();

        auto& shutdown = ShutdownToken::instance();
        writer_thread_ = std::thread([this] { writer_loop(); });
        rest_thread_ = std::thread([this] { rest_loop(); });

        int64_t last_reload = 0;
        int64_t last_ops = 0;
        int64_t last_minute = timeutil::floor_minute(timeutil::now_ms());
        while (!shutdown.wait_for(std::chrono::seconds(1))) {
            const int64_t now = timeutil::now_ms();
            if (now - last_reload >= reload_interval_sec_ * 1000 || last_reload == 0) {
                reload_instruments();
                last_reload = now;
            }
            const int64_t minute = timeutil::floor_minute(now);
            if (minute > last_minute) {
                roll_minute(last_minute);
                last_minute = minute;
            }
            if (now - last_ops >= 30000) {
                update_ops_state();
                last_ops = now;
            }
        }

        stop_sessions();
        {
            std::lock_guard lk(write_mu_);
            write_stop_ = true;
        }
        write_cv_.notify_all();
        if (writer_thread_.joinable()) writer_thread_.join();
        if (rest_thread_.joinable()) rest_thread_.join();
        js_->stop();
        nats_->drain(3000);
        log::info("service_stopped", {{"component", spec_.service_name}});
        return 0;
    }

private:
    // ---- instruments / sessions --------------------------------------------
    void make_adapter() {
        auto& shutdown = ShutdownToken::instance();
        auto stop_fn = [&shutdown] { return shutdown.stop_requested(); };
        switch (spec_.exchange_id) {
            case exchanges::kVenueBinanceUsdm:
                binance_cfg_ = exchanges::binance::parse_config(ctx_.service);
                adapter_ = std::make_unique<exchanges::binance::BinanceUsdmMarketDataAdapter>(
                    binance_cfg_, stop_fn);
                break;
            case exchanges::kVenueBybit:
                bybit_cfg_ = exchanges::bybit::parse_config(ctx_.service);
                adapter_ = std::make_unique<exchanges::bybit::BybitLinearMarketDataAdapter>(
                    bybit_cfg_, stop_fn);
                break;
            case exchanges::kVenueOkx:
                okx_cfg_ = exchanges::okx::parse_config(ctx_.service);
                adapter_ = std::make_unique<exchanges::okx::OkxUsdtSwapMarketDataAdapter>(
                    okx_cfg_, stop_fn);
                break;
            case exchanges::kVenueBitget: {
                // Bitget takes its market data over WebSocket, so this runtime
                // long had no REST adapter at all. It still needs one: funding
                // SETTLEMENT history is REST-only on every venue, and without it
                // Bitget was the single venue with no settlement history, which
                // left AltTech unable to derive its funding interval and
                // `bitget_funding_deviation` permanently unset.
                // The venue's settings live under the component's `bitget:`
                // subsection, the same shape instrument_catalog parses. Every
                // REST key has a default, so a section carrying only WS settings
                // still yields a usable REST client.
                auto cfg = config::parse_bitget_rest(ctx_.service["bitget"]);
                if (!cfg) {
                    log::warn("bitget_rest_config_invalid",
                              {{"message", cfg.error().message},
                               {"effect", "funding settlement history will not be collected"}});
                    break;
                }
                adapter_ = std::make_unique<exchanges::bitget::BitgetMarketDataAdapter>(*cfg,
                                                                                       stop_fn);
                break;
            }
            default:
                break;
        }
    }

    void reload_instruments() {
        std::vector<RuntimeInstrument> fresh;
        auto res = pool_.with_txn([&](pqxx::work& txn) {
            const auto rows = txn.exec_params(
                "SELECT ei.exchange_symbol, ei.instrument_id, i.base_asset, "
                "       (rca.base_asset IS NOT NULL) AS is_core, "
                "       COALESCE((SELECT isv.spec->>'contract_value' "
                "                 FROM instrument_spec_versions isv "
                "                 WHERE isv.exchange_instrument_id = ei.id "
                "                 ORDER BY isv.version DESC LIMIT 1), '1') "
                "FROM exchange_instruments ei "
                "JOIN instruments i ON i.id = ei.instrument_id "
                "LEFT JOIN retention_core_assets rca ON rca.base_asset = i.base_asset "
                "WHERE ei.exchange_id = $1 AND ei.is_whitelisted AND ei.status = 'active' "
                "ORDER BY ei.exchange_symbol",
                spec_.exchange_id);
            for (const auto& r : rows) {
                RuntimeInstrument ri;
                ri.symbol = r[0].as<std::string>();
                ri.instrument_id = r[1].as<int64_t>();
                ri.base_asset = r[2].as<std::string>();
                ri.core = r[3].as<bool>();
                auto cv = Decimal::parse(r[4].as<std::string>("1"));
                ri.contract_value = cv ? *cv : Decimal{1, 0};
                fresh.push_back(std::move(ri));
            }
        });
        if (!res) {
            log::warn("runtime_instrument_load_failed",
                      {{"venue", spec_.code}, {"message", res.error().message}});
            return;
        }

        std::vector<std::string> symbols;
        symbols.reserve(fresh.size());
        for (const auto& ri : fresh) symbols.push_back(ri.symbol);
        bool changed = false;
        {
            std::lock_guard lk(inst_mu_);
            if (symbols != known_symbols_) changed = true;
            instruments_.clear();
            for (auto& ri : fresh) instruments_.emplace(ri.symbol, ri);
            known_symbols_ = symbols;
        }
        if (!changed && sessions_started_) return;

        log::info("runtime_universe", {{"venue", spec_.code}, {"instruments", symbols.size()}});
        rebuild_sessions(symbols, fresh);
    }

    void rebuild_sessions(const std::vector<std::string>& symbols,
                          const std::vector<RuntimeInstrument>& fresh) {
        stop_sessions();
        {
            std::lock_guard lk(book_mu_);
            books_.clear();
        }
        if (symbols.empty()) {
            sessions_started_ = false;
            return; // nothing eligible; keep polling for universe changes
        }

        exchanges::VenueWsSession::Callbacks cb;
        cb.on_events = [this](exchanges::VenueEvents&& ev, int64_t recv_ms,
                              const std::string& conn_id) {
            handle_events(std::move(ev), recv_ms, conn_id);
        };
        cb.on_connection_event = [this](const std::string& kind, const std::string& conn_id,
                                        const std::string& detail) {
            publish_gateway_event(kind, conn_id, detail);
        };

        exchanges::VenueWsSession::Options opts;
        std::unique_ptr<exchanges::VenueWsProtocol> proto;
        std::unique_ptr<exchanges::VenueWsProtocol> proto2;
        switch (spec_.exchange_id) {
            case exchanges::kVenueBinanceUsdm:
                opts.stale_timeout_sec = binance_cfg_.stale_timeout_sec;
                proto = std::make_unique<exchanges::binance::BinanceWsProtocol>(
                    binance_cfg_, exchanges::binance::BinanceWsProtocol::StreamGroup::market);
                proto2 = std::make_unique<exchanges::binance::BinanceWsProtocol>(
                    binance_cfg_, exchanges::binance::BinanceWsProtocol::StreamGroup::depth);
                break;
            case exchanges::kVenueBybit:
                opts.stale_timeout_sec = bybit_cfg_.stale_timeout_sec;
                proto = std::make_unique<exchanges::bybit::BybitWsProtocol>(bybit_cfg_);
                break;
            case exchanges::kVenueOkx: {
                opts.stale_timeout_sec = okx_cfg_.stale_timeout_sec;
                std::map<std::string, Decimal> ct;
                for (const auto& ri : fresh) ct.emplace(ri.symbol, ri.contract_value);
                proto = std::make_unique<exchanges::okx::OkxPublicWsProtocol>(okx_cfg_,
                                                                              std::move(ct));
                proto2 = std::make_unique<exchanges::okx::OkxBusinessWsProtocol>(okx_cfg_);
                break;
            }
            case exchanges::kVenueBitget: {
                const auto node = ctx_.service["bitget"] ? ctx_.service["bitget"] : ctx_.service;
                const std::string url = config::opt_str(node, "ws_public_url",
                                                        "wss://ws.bitget.com/v2/ws/public");
                const std::string product =
                    config::opt_str(node, "product_type", "USDT-FUTURES");
                proto = std::make_unique<exchanges::bitget::BitgetTickerWsProtocol>(url, product);
                break;
            }
            default:
                return;
        }

        session_ = std::make_unique<exchanges::VenueWsSession>(
            std::string(spec_.code) + "-ws", std::move(proto), opts);
        session_->start(symbols, cb);
        if (proto2) {
            session2_ = std::make_unique<exchanges::VenueWsSession>(
                std::string(spec_.code) + "-ws-2", std::move(proto2), opts);
            session2_->start(symbols, cb);
        }
        sessions_started_ = true;
    }

    void stop_sessions() {
        if (session_) session_->stop();
        if (session2_) session2_->stop();
        session_.reset();
        session2_.reset();
    }

    std::optional<RuntimeInstrument> lookup(const std::string& symbol) {
        std::lock_guard lk(inst_mu_);
        const auto it = instruments_.find(symbol);
        if (it == instruments_.end()) return std::nullopt;
        return it->second;
    }

    // ---- io-thread event handling ------------------------------------------
    void handle_events(exchanges::VenueEvents&& ev, int64_t recv_ms, const std::string& conn_id) {
        for (auto& t : ev.trades) handle_trade(t, recv_ms, conn_id);
        for (auto& c : ev.candles) handle_candle(c, recv_ms, conn_id);
        for (auto& t : ev.tickers) handle_ticker(t, recv_ms, conn_id);
        for (auto& b : ev.books) handle_book(b, recv_ms, conn_id);
    }

    void handle_trade(exchanges::VenueTradeEvent& t, int64_t recv_ms, const std::string& conn_id) {
        auto ri = lookup(t.symbol);
        if (!ri) return;
        if (t.trade.exchange_ts_ms > 0) note_clock(t.trade.exchange_ts_ms, recv_ms);
        touch(t.symbol, [&](ContextState& s) { s.seen_trade = recv_ms; });

        messaging::EnvelopeMeta meta;
        meta.event_type = "md.trade";
        meta.exchange_id = spec_.exchange_id;
        meta.instrument_id = ri->instrument_id;
        meta.exchange_symbol = t.symbol;
        meta.exchange_timestamp_ms = t.trade.exchange_ts_ms;
        meta.trace_id = conn_id;
        meta.connection_id = conn_id;
        meta.source_channel = "trade";
        meta.local_receive_ms = recv_ms;
        meta.source_service = spec_.service_name;
        auto env = messaging::make_envelope(meta);
        messaging::to_proto(t.trade, env.mutable_trade());
        (void)nats_->publish(subjects::trade(spec_.code, t.symbol), messaging::serialize(env));

        if (PersistenceGate::instance().allow_raw_trades() &&
            (ri->core || PersistenceGate::instance().mode() != PersistenceMode::core_only) &&
            t.trade.exchange_ts_ms > 0 && !t.trade.exchange_trade_id.empty() &&
            t.trade.side != TradeSide::unspecified) {
            std::lock_guard lk(write_mu_);
            if (pending_trades_.size() < max_pending_rows_) {
                pending_trades_.push_back(PendingTrade{ri->instrument_id, ri->core,
                                                      std::move(t.trade)});
            }
        }
    }

    void handle_candle(exchanges::VenueCandleEvent& c, int64_t recv_ms,
                       const std::string& conn_id) {
        auto ri = lookup(c.symbol);
        if (!ri) return;
        touch(c.symbol, [&](ContextState& s) { s.seen_candle = recv_ms; });

        messaging::EnvelopeMeta meta;
        meta.exchange_id = spec_.exchange_id;
        meta.instrument_id = ri->instrument_id;
        meta.exchange_symbol = c.symbol;
        meta.exchange_timestamp_ms = recv_ms;
        meta.trace_id = conn_id;
        meta.connection_id = conn_id;
        meta.source_channel = "kline";
        meta.local_receive_ms = recv_ms;
        meta.source_service = spec_.service_name;
        if (c.candle.is_closed) {
            meta.event_type = "bar.closed";
            meta.message_id = messaging::candle_msg_id(spec_.code, c.symbol, Timeframe::m1,
                                                       c.candle.open_time_ms);
            auto env = messaging::make_envelope(meta);
            messaging::to_proto(c.candle, env.mutable_candle());
            js_->enqueue({subjects::bar_closed(spec_.code, c.symbol, Timeframe::m1),
                          messaging::serialize(env), env.message_id(), &bars_published_});
        } else {
            meta.event_type = "md.candle.update";
            auto env = messaging::make_envelope(meta);
            messaging::to_proto(c.candle, env.mutable_candle());
            (void)nats_->publish(subjects::candle_update(spec_.code, c.symbol),
                                 messaging::serialize(env));
        }
    }

    void handle_ticker(exchanges::VenueTickerEvent& t, int64_t recv_ms,
                       const std::string& conn_id) {
        auto ri = lookup(t.symbol);
        if (!ri) return;
        if (t.exchange_ms > 0) note_clock(t.exchange_ms, recv_ms);

        // Merge into the last-value context state and decide throttled publishes.
        std::vector<messaging::JsPublishQueue::Item> to_publish;
        {
            std::lock_guard lk(ctx_mu_);
            auto& s = context_[t.symbol];
            const int64_t now = recv_ms;
            if (t.mark_price) { s.mark = t.mark_price; s.mask |= 1; s.seen_mark = now; }
            if (t.index_price) { s.index = t.index_price; s.mask |= 2; }
            if (t.funding_rate) { s.funding = t.funding_rate; s.mask |= 4; }
            if (t.next_funding_time_ms > 0) s.next_funding_ms = t.next_funding_time_ms;
            if (t.open_interest_base || t.open_interest_quote) {
                if (t.open_interest_base) s.oi_base = t.open_interest_base;
                if (t.open_interest_quote) s.oi_quote = t.open_interest_quote;
                s.mask |= 8;
                s.seen_oi = now;
            }
            if (t.quote_volume_24h) { s.turnover_24h = t.quote_volume_24h; s.mask |= 32; }
            if (t.last_price) s.seen_ticker = now;
            if (t.bid_price && t.ask_price) {
                s.best_bid = t.bid_price;
                s.best_ask = t.ask_price;
                s.mask |= 16;
            }

            const auto make_meta = [&](const char* kind) {
                messaging::EnvelopeMeta meta;
                meta.event_type = std::string("md.") + kind;
                meta.exchange_id = spec_.exchange_id;
                meta.instrument_id = ri->instrument_id;
                meta.exchange_symbol = t.symbol;
                meta.exchange_timestamp_ms = t.exchange_ms;
                meta.trace_id = conn_id;
                meta.connection_id = conn_id;
                meta.source_channel = "ticker";
                meta.local_receive_ms = recv_ms;
                meta.source_service = spec_.service_name;
                meta.message_id = std::string(kind) + ":" + spec_.code + ":" + t.symbol + ":" +
                                  std::to_string(now);
                return meta;
            };
            if (t.last_price && now - s.pub_ticker >= ticker_pub_interval_ms_) {
                s.pub_ticker = now;
                auto env = messaging::make_envelope(make_meta("ticker"));
                auto* mt = env.mutable_market_ticker();
                *mt->mutable_last_price() = messaging::to_proto(*t.last_price);
                if (t.open_24h) *mt->mutable_open_24h() = messaging::to_proto(*t.open_24h);
                if (t.high_24h) *mt->mutable_high_24h() = messaging::to_proto(*t.high_24h);
                if (t.low_24h) *mt->mutable_low_24h() = messaging::to_proto(*t.low_24h);
                if (t.base_volume_24h)
                    *mt->mutable_base_volume_24h() = messaging::to_proto(*t.base_volume_24h);
                if (t.quote_volume_24h)
                    *mt->mutable_quote_volume_24h() = messaging::to_proto(*t.quote_volume_24h);
                if (t.bid_price) *mt->mutable_bid_price() = messaging::to_proto(*t.bid_price);
                if (t.ask_price) *mt->mutable_ask_price() = messaging::to_proto(*t.ask_price);
                to_publish.push_back({subjects::market_context(spec_.code, t.symbol, "ticker"),
                                      messaging::serialize(env), env.message_id(), nullptr});
            }
            if (t.mark_price && now - s.pub_mark >= context_pub_interval_ms_) {
                s.pub_mark = now;
                auto env = messaging::make_envelope(make_meta("mark"));
                *env.mutable_mark_price()->mutable_price() = messaging::to_proto(*t.mark_price);
                to_publish.push_back({subjects::market_context(spec_.code, t.symbol, "mark"),
                                      messaging::serialize(env), env.message_id(), nullptr});
            }
            if (t.index_price && now - s.pub_index >= context_pub_interval_ms_) {
                s.pub_index = now;
                auto env = messaging::make_envelope(make_meta("index"));
                *env.mutable_index_price()->mutable_price() = messaging::to_proto(*t.index_price);
                to_publish.push_back({subjects::market_context(spec_.code, t.symbol, "index"),
                                      messaging::serialize(env), env.message_id(), nullptr});
            }
            if (t.funding_rate && now - s.pub_funding >= context_pub_interval_ms_) {
                s.pub_funding = now;
                auto env = messaging::make_envelope(make_meta("funding"));
                auto* fr = env.mutable_funding_rate();
                *fr->mutable_rate() = messaging::to_proto(*t.funding_rate);
                if (s.next_funding_ms > 0) fr->set_next_funding_time_ms(s.next_funding_ms);
                to_publish.push_back({subjects::market_context(spec_.code, t.symbol, "funding"),
                                      messaging::serialize(env), env.message_id(), nullptr});
            }
            if ((t.open_interest_base || t.open_interest_quote) &&
                now - s.pub_oi >= context_pub_interval_ms_) {
                s.pub_oi = now;
                auto env = messaging::make_envelope(make_meta("open_interest"));
                auto* oi = env.mutable_open_interest();
                if (s.oi_base) *oi->mutable_base() = messaging::to_proto(*s.oi_base);
                if (s.oi_quote) *oi->mutable_quote() = messaging::to_proto(*s.oi_quote);
                to_publish.push_back(
                    {subjects::market_context(spec_.code, t.symbol, "open_interest"),
                     messaging::serialize(env), env.message_id(), nullptr});
            }
            // Venues without a book subscription here (bitget context runtime)
            // feed the BBO minute accumulator from the ticker's bid/ask.
            if (spec_.context_only && t.bid_price && t.ask_price) {
                update_bbo_locked(s, *t.bid_price, *t.ask_price, now);
                if (now - s.pub_bbo >= bbo_pub_interval_ms_) {
                    s.pub_bbo = now;
                    auto env = messaging::make_envelope(make_meta("bbo"));
                    auto* bb = env.mutable_best_bid_ask();
                    *bb->mutable_bid_price() = messaging::to_proto(*t.bid_price);
                    *bb->mutable_ask_price() = messaging::to_proto(*t.ask_price);
                    if (t.bid_size) *bb->mutable_bid_size() = messaging::to_proto(*t.bid_size);
                    if (t.ask_size) *bb->mutable_ask_size() = messaging::to_proto(*t.ask_size);
                    to_publish.push_back({subjects::market_context(spec_.code, t.symbol, "bbo"),
                                          messaging::serialize(env), env.message_id(), nullptr});
                }
            }
        }
        for (auto& item : to_publish) js_->enqueue(std::move(item));
    }

    void handle_book(exchanges::VenueBookEvent& b, int64_t recv_ms, const std::string& conn_id) {
        auto ri = lookup(b.symbol);
        if (!ri) return;
        if (b.update.exchange_ms > 0) note_clock(b.update.exchange_ms, recv_ms);

        BookState* st = book_state(b.symbol);
        BookApplyResult result;
        std::vector<Level> top_bids, top_asks;
        int64_t seq = 0;
        std::optional<Decimal> ofi;
        uint64_t book_epoch = 0, delta_counter = 0;
        {
            std::lock_guard lk(st->mu);
            result = st->book->apply(b.update);
            seq = st->book->last_sequence();
            ofi = st->book->last_ofi();
            book_epoch = st->book->epoch();
            delta_counter = st->book->delta_counter();
            if (result == BookApplyResult::ok || result == BookApplyResult::reset) {
                st->desynced = false;
                top_bids = st->book->top_bids(static_cast<size_t>(snapshot_depth_));
                top_asks = st->book->top_asks(static_cast<size_t>(snapshot_depth_));
            }
        }

        switch (result) {
            case BookApplyResult::desync_gap:
            case BookApplyResult::desync_checksum:
            case BookApplyResult::invalid:
                on_book_desync(b.symbol, *ri, result, conn_id);
                return;
            case BookApplyResult::crossed:
                touch(b.symbol, [&](ContextState& s) { ++s.crossed; });
                on_book_desync(b.symbol, *ri, result, conn_id);
                return; // do not publish a crossed book as valid
            case BookApplyResult::empty:
                on_book_desync(b.symbol, *ri, result, conn_id);
                return;
            case BookApplyResult::waiting:
                // Binance builds its book from a REST depth snapshot + the
                // pu-chained deltas; deltas before the first snapshot keep the
                // book in `waiting`, so request the snapshot here (throttled).
                if (spec_.sequencing == exchanges::LocalBook::Sequencing::binance_futures) {
                    request_book_snapshot(b.symbol);
                }
                return;
            case BookApplyResult::ok:
            case BookApplyResult::reset:
                break;
        }
        touch(b.symbol, [&](ContextState& s) { s.seen_book = recv_ms; });

        // Live publishing: deltas as received, snapshots as bounded top-depth.
        messaging::EnvelopeMeta meta;
        meta.exchange_id = spec_.exchange_id;
        meta.instrument_id = ri->instrument_id;
        meta.exchange_symbol = b.symbol;
        meta.exchange_timestamp_ms = b.update.exchange_ms;
        meta.trace_id = conn_id;
        meta.connection_id = conn_id;
        meta.source_channel = "book";
        meta.sequence = b.update.sequence;
        meta.local_receive_ms = recv_ms;
        meta.source_service = spec_.service_name;
        if (result == BookApplyResult::reset) {
            meta.event_type = "md.book.snapshot";
            auto env = messaging::make_envelope(meta);
            auto* snap = env.mutable_order_book_snapshot();
            for (const auto& l : top_bids) {
                auto* lv = snap->add_bids();
                *lv->mutable_price() = messaging::to_proto(l.price);
                *lv->mutable_size() = messaging::to_proto(l.size);
            }
            for (const auto& l : top_asks) {
                auto* lv = snap->add_asks();
                *lv->mutable_price() = messaging::to_proto(l.price);
                *lv->mutable_size() = messaging::to_proto(l.size);
            }
            snap->set_sequence(seq);
            snap->set_exchange_time_ms(b.update.exchange_ms);
            snap->set_depth(snapshot_depth_);
            snap->set_is_snapshot(true);
            if (b.update.has_checksum) snap->set_checksum(b.update.checksum);
            (void)nats_->publish(subjects::book_snapshot(spec_.code, b.symbol),
                                 messaging::serialize(env));
        } else {
            meta.event_type = "md.book.delta";
            auto env = messaging::make_envelope(meta);
            auto* delta = env.mutable_order_book_delta();
            for (const auto& l : b.update.bids) {
                auto* lv = delta->add_bids();
                *lv->mutable_price() = messaging::to_proto(l.price);
                *lv->mutable_size() = messaging::to_proto(l.size);
            }
            for (const auto& l : b.update.asks) {
                auto* lv = delta->add_asks();
                *lv->mutable_price() = messaging::to_proto(l.price);
                *lv->mutable_size() = messaging::to_proto(l.size);
            }
            delta->set_first_update_id(b.update.first_update_id);
            delta->set_sequence(b.update.sequence);
            if (b.update.prev_sequence >= 0) delta->set_previous_sequence(b.update.prev_sequence);
            delta->set_exchange_time_ms(b.update.exchange_ms);
            if (b.update.has_checksum) delta->set_checksum(b.update.checksum);
            if (ofi) *delta->mutable_ofi_base() = messaging::to_proto(*ofi);
            delta->set_book_epoch(book_epoch);
            delta->set_delta_counter(delta_counter);
            if (!top_bids.empty() && !top_asks.empty()) {
                if (auto depth = top_bids.front().size.add(top_asks.front().size);
                    depth && !depth->is_negative() && !depth->is_zero()) {
                    *delta->mutable_bbo_depth_base() = messaging::to_proto(*depth);
                }
            }
            (void)nats_->publish(subjects::book_delta(spec_.code, b.symbol),
                                 messaging::serialize(env));
        }

        if (top_bids.empty() || top_asks.empty()) return;
        const Decimal best_bid = top_bids.front().price;
        const Decimal best_ask = top_asks.front().price;

        // Throttled BBO to JetStream + minute accumulator.
        bool publish_bbo = false;
        {
            std::lock_guard lk(ctx_mu_);
            auto& s = context_[b.symbol];
            s.best_bid = best_bid;
            s.best_ask = best_ask;
            s.mask |= 16;
            update_bbo_locked(s, best_bid, best_ask, recv_ms);
            if (recv_ms - s.pub_bbo >= bbo_pub_interval_ms_) {
                s.pub_bbo = recv_ms;
                publish_bbo = true;
            }
        }
        if (publish_bbo) {
            meta.event_type = "md.bbo";
            meta.message_id = "bbo:" + std::string(spec_.code) + ":" + b.symbol + ":" +
                              std::to_string(recv_ms);
            auto env = messaging::make_envelope(meta);
            auto* bb = env.mutable_best_bid_ask();
            *bb->mutable_bid_price() = messaging::to_proto(best_bid);
            *bb->mutable_bid_size() = messaging::to_proto(top_bids.front().size);
            *bb->mutable_ask_price() = messaging::to_proto(best_ask);
            *bb->mutable_ask_size() = messaging::to_proto(top_asks.front().size);
            js_->enqueue({subjects::market_context(spec_.code, b.symbol, "bbo"),
                          messaging::serialize(env), env.message_id(), nullptr});
        }

        // Sampled top-depth persistence (class-aware interval, budget-gated).
        const int64_t interval = ri->core ? core_interval_ms_ : other_interval_ms_;
        const bool sample_due = recv_ms - st->last_snapshot_row_ms >= interval;
        if (sample_due && !PersistenceGate::instance().allow_book_snapshots()) {
            // A due sample discarded by the storage budget. Folded into the
            // condition below it wrote nothing at all, which is how four venues
            // of order-book data went unrecorded without one line saying so.
            const int64_t shed = books_shed_.fetch_add(1) + 1;
            const int64_t now = timeutil::now_ms();
            int64_t last = last_shed_log_ms_.load();
            if (now - last >= 60'000 && last_shed_log_ms_.compare_exchange_strong(last, now)) {
                log::warn("book_snapshots_shed",
                          {{"venue", spec_.code},
                           {"total_shed", shed},
                           {"persistence_mode",
                            std::string(persistence_mode_label(PersistenceGate::instance().mode()))},
                           {"reason", "storage budget: raise hard_database_limit_bytes or "
                                      "shorten retention; see docs/STORAGE_BUDGET.md"}});
            }
        }
        if (sample_due && PersistenceGate::instance().allow_book_snapshots()) {
            st->last_snapshot_row_ms = recv_ms;
            PendingSnapshot row;
            row.instrument_id = ri->instrument_id;
            row.core = ri->core;
            row.snapshot_time_ms =
                b.update.exchange_ms > 0 ? b.update.exchange_ms : recv_ms;
            row.seq = seq;
            row.bids = std::move(top_bids);
            row.asks = std::move(top_asks);
            std::lock_guard lk(write_mu_);
            if (pending_snapshots_.size() < max_pending_rows_) {
                pending_snapshots_.push_back(std::move(row));
            }
        }
    }

    // Enqueue a REST depth-snapshot fetch for one symbol, at most every 2s.
    void request_book_snapshot(const std::string& symbol) {
        BookState* st = book_state(symbol);
        const int64_t now = timeutil::now_ms();
        {
            std::lock_guard lk(st->mu);
            if (now - st->last_resync_ms < 2000) return;
            st->last_resync_ms = now;
        }
        enqueue_resync(symbol);
    }

    // Queue a REST snapshot fetch, skipping symbols already queued or in
    // flight. Without this the 250 ms delta stream re-queues the same symbol
    // faster than the REST worker drains it, and the burst trips the venue's
    // request-weight limit (HTTP 429), which in turn keeps the book desynced.
    void enqueue_resync(const std::string& symbol) {
        std::lock_guard lk(resync_mu_);
        if (!resync_queued_.insert(symbol).second) return;
        resync_queue_.push_back(symbol);
    }

    void on_book_desync(const std::string& symbol, const RuntimeInstrument& ri,
                        BookApplyResult result, const std::string& conn_id) {
        BookState* st = book_state(symbol);
        const int64_t now = timeutil::now_ms();
        bool first = false;
        {
            std::lock_guard lk(st->mu);
            first = !st->desynced;
            st->desynced = true;
            if (now - st->last_resync_ms < 2000) return; // resync already pending
            st->last_resync_ms = now;
        }
        touch(symbol, [&](ContextState& s) { ++s.seq_gaps; });
        if (first) {
            const char* reason = result == BookApplyResult::desync_checksum ? "checksum mismatch" :
                result == BookApplyResult::crossed ? "crossed book" :
                result == BookApplyResult::empty ? "empty book" :
                result == BookApplyResult::invalid ? "invalid book level" : "sequence gap";
            publish_integrity(symbol, ri, "book_desynced",
                              reason);
            log::warn("book_desynced", {{"venue", spec_.code},
                                        {"symbol", symbol},
                                        {"connection_id", conn_id},
                                        {"reason", reason}});
        }
        if (spec_.sequencing == exchanges::LocalBook::Sequencing::binance_futures) {
            enqueue_resync(symbol);
        } else if (session_) {
            // Bybit/OKX: the venue resends a snapshot after resubscribe.
            switch (spec_.exchange_id) {
                case exchanges::kVenueBybit:
                    session_->send_frames(
                        exchanges::bybit::BybitWsProtocol(bybit_cfg_).resubscribe_book_frames(
                            symbol));
                    break;
                case exchanges::kVenueOkx:
                    session_->send_frames(
                        exchanges::okx::OkxPublicWsProtocol(okx_cfg_, {})
                            .resubscribe_book_frames(symbol));
                    break;
                default:
                    break;
            }
        }
    }

    BookState* book_state(const std::string& symbol) {
        std::lock_guard lk(book_mu_);
        auto it = books_.find(symbol);
        if (it == books_.end()) {
            auto st = std::make_unique<BookState>();
            // A process restart may reuse the logical WS connection id and
            // venue sequence. Inject a fresh epoch identity into the pure book
            // so consumers cannot join the two runs as one continuous stream.
            std::random_device entropy;
            const uint64_t epoch_seed = ((static_cast<uint64_t>(entropy()) << 32U) ^ entropy()) & 0x7fffffffffffffffULL;
            st->book = std::make_unique<exchanges::LocalBook>(spec_.sequencing, epoch_seed);
            it = books_.emplace(symbol, std::move(st)).first;
        }
        return it->second.get();
    }

    template <typename Fn>
    void touch(const std::string& symbol, Fn&& fn) {
        std::lock_guard lk(ctx_mu_);
        fn(context_[symbol]);
    }

    void update_bbo_locked(ContextState& s, const Decimal& bid, const Decimal& ask, int64_t) {
        auto& b = s.bbo;
        if (!b.bid_first) b.bid_first = bid;
        b.bid_last = bid;
        if (!b.bid_min || bid < *b.bid_min) b.bid_min = bid;
        if (!b.bid_max || *b.bid_max < bid) b.bid_max = bid;
        if (!b.ask_first) b.ask_first = ask;
        b.ask_last = ask;
        if (!b.ask_min || ask < *b.ask_min) b.ask_min = ask;
        if (!b.ask_max || *b.ask_max < ask) b.ask_max = ask;
        const Decimal neg_bid{-bid.mantissa(), bid.scale()};
        if (auto spread = ask.add(neg_bid)) {
            if (!b.spread_min || *spread < *b.spread_min) b.spread_min = *spread;
            if (!b.spread_max || *b.spread_max < *spread) b.spread_max = *spread;
        }
        ++b.samples;
    }

    void note_clock(int64_t exchange_ms, int64_t recv_ms) {
        const int64_t offset = exchange_ms - recv_ms;
        int64_t prev = clock_offset_ms_.load(std::memory_order_relaxed);
        // EWMA (1/16) without locking; races only cost a sample.
        clock_offset_ms_.store(prev + (offset - prev) / 16, std::memory_order_relaxed);
        clock_samples_.fetch_add(1, std::memory_order_relaxed);
    }

    // ---- publishing helpers -------------------------------------------------
    void publish_gateway_event(const std::string& kind, const std::string& conn_id,
                               const std::string& detail) {
        if (kind == "disconnected") reconnects_.fetch_add(1, std::memory_order_relaxed);
        messaging::EnvelopeMeta meta;
        meta.event_type = "gateway." + kind;
        meta.exchange_id = spec_.exchange_id;
        meta.trace_id = conn_id;
        meta.connection_id = conn_id;
        meta.source_service = spec_.service_name;
        meta.message_id = "gw:" + std::string(spec_.code) + ":" + kind + ":" + conn_id + ":" +
                          std::to_string(timeutil::now_ms());
        auto env = messaging::make_envelope(meta);
        auto* ev = env.mutable_gateway_event();
        ev->set_type(kind == "connected"      ? altspotter::v1::GATEWAY_EVENT_CONNECTED
                     : kind == "disconnected" ? altspotter::v1::GATEWAY_EVENT_DISCONNECTED
                     : kind == "degraded"     ? altspotter::v1::GATEWAY_EVENT_DEGRADED
                                              : altspotter::v1::GATEWAY_EVENT_RECOVERED);
        ev->set_connection_id(conn_id);
        ev->set_detail(detail);
        js_->enqueue({subjects::gateway_event(spec_.code, kind), messaging::serialize(env),
                      env.message_id(), nullptr});
    }

    void publish_integrity(const std::string& symbol, const RuntimeInstrument& ri,
                           const char* kind, const std::string& detail) {
        messaging::EnvelopeMeta meta;
        meta.event_type = std::string("integrity.") + kind;
        meta.exchange_id = spec_.exchange_id;
        meta.instrument_id = ri.instrument_id;
        meta.exchange_symbol = symbol;
        meta.source_service = spec_.service_name;
        meta.message_id = std::string(kind) + ":" + spec_.code + ":" + symbol + ":" +
                          std::to_string(timeutil::now_ms() / 1000);
        auto env = messaging::make_envelope(meta);
        auto* ev = env.mutable_integrity_event();
        ev->set_state(altspotter::v1::HEALTH_DEGRADED);
        ev->set_detail(detail);
        js_->enqueue({subjects::integrity_event(spec_.code, symbol, kind),
                      messaging::serialize(env), env.message_id(), nullptr});
    }

    // ---- writer thread ------------------------------------------------------
    void writer_loop() {
        auto& shutdown = ShutdownToken::instance();
        for (;;) {
            std::vector<PendingTrade> trades;
            std::vector<PendingSnapshot> snaps;
            {
                std::unique_lock lk(write_mu_);
                write_cv_.wait_for(lk, std::chrono::milliseconds(flush_interval_ms_), [&] {
                    return write_stop_ || pending_trades_.size() > 5000;
                });
                trades.swap(pending_trades_);
                snaps.swap(pending_snapshots_);
                if (write_stop_ && trades.empty() && snaps.empty()) return;
            }
            if (!trades.empty()) flush_trades(trades);
            if (!snaps.empty()) flush_snapshots(snaps);
            if (shutdown.stop_requested()) {
                std::lock_guard lk(write_mu_);
                if (write_stop_ && pending_trades_.empty() && pending_snapshots_.empty()) return;
            }
        }
    }

    void flush_trades(const std::vector<PendingTrade>& trades) {
        persistence::with_txn_logged(pool_, "runtime_trades_insert", [&](pqxx::work& txn) {
            std::ostringstream sql;
            sql << "INSERT INTO market_trades_raw (exchange_id, instrument_id, trade_time, "
                   "trade_id, price, size, side, retention_class) VALUES ";
            bool first = true;
            for (const auto& t : trades) {
                sql << (first ? "(" : ",(");
                first = false;
                sql << spec_.exchange_id << ',' << t.instrument_id << ",to_timestamp("
                    << t.trade.exchange_ts_ms << "::double precision/1000.0),"
                    << txn.quote(t.trade.exchange_trade_id) << ','
                    << txn.quote(t.trade.price.to_string()) << "::numeric,"
                    << txn.quote(t.trade.size.to_string()) << "::numeric,"
                    << (t.trade.side == TradeSide::buy ? "'buy'" : "'sell'") << ','
                    << (t.core ? 1 : 0) << ')';
            }
            sql << " ON CONFLICT DO NOTHING";
            txn.exec(sql.str());
        });
    }

    void flush_snapshots(const std::vector<PendingSnapshot>& snaps) {
        persistence::with_txn_logged(pool_, "runtime_snapshots_insert", [&](pqxx::work& txn) {
            for (const auto& s : snaps) {
                if (s.bids.empty() || s.asks.empty()) continue;
                const double bid = std::stod(s.bids.front().price.to_string());
                const double ask = std::stod(s.asks.front().price.to_string());
                if (bid <= 0 || ask <= 0 || ask < bid) continue;
                const double mid = (bid + ask) / 2.0;
                double bd = 0, ad = 0, bsz = 0, asz = 0;
                std::string bids_j = "[", asks_j = "[";
                for (size_t i = 0; i < s.bids.size(); ++i) {
                    const auto& l = s.bids[i];
                    bd += std::stod(l.size.to_string());
                    if (i == 0) bsz = std::stod(l.size.to_string());
                    bids_j += (i ? ",[" : "[") + std::string("\"") + l.price.to_string() +
                              "\",\"" + l.size.to_string() + "\"]";
                }
                bids_j += "]";
                for (size_t i = 0; i < s.asks.size(); ++i) {
                    const auto& l = s.asks[i];
                    ad += std::stod(l.size.to_string());
                    if (i == 0) asz = std::stod(l.size.to_string());
                    asks_j += (i ? ",[" : "[") + std::string("\"") + l.price.to_string() +
                              "\",\"" + l.size.to_string() + "\"]";
                }
                asks_j += "]";
                const double micro = (bsz + asz) > 0 ? (bid * asz + ask * bsz) / (bsz + asz) : mid;
                const double spread_bps = mid > 0 ? (ask - bid) / mid * 1e4 : 0.0;
                const double imb = (bd + ad) > 0 ? (bd - ad) / (bd + ad) : 0.0;
                txn.exec_params(
                    "INSERT INTO market_orderbook_snapshots "
                    "(exchange_id, instrument_id, snapshot_time, seq, best_bid, best_ask, mid, "
                    " microprice, spread_bps, bid_depth_n, ask_depth_n, imbalance_n, levels, "
                    " bids, asks, retention_class) "
                    "VALUES ($1,$2,to_timestamp($3::double precision/1000.0),$4,$5,$6,$7,$8,$9,"
                    "$10,$11,$12,$13,$14::jsonb,$15::jsonb,$16) ON CONFLICT DO NOTHING",
                    spec_.exchange_id, s.instrument_id, s.snapshot_time_ms, s.seq, bid, ask, mid,
                    micro, spread_bps, bd, ad, imb,
                    static_cast<int>(std::max(s.bids.size(), s.asks.size())), bids_j, asks_j,
                    s.core ? 1 : 0);
            }
        });
    }

    // Per-minute roll: derivatives context + BBO minute rows.
    void roll_minute(int64_t minute_open_ms) {
        struct CtxRow {
            std::string symbol;
            int64_t instrument_id;
            ContextState state;
        };
        std::vector<CtxRow> rows;
        {
            std::lock_guard lk(ctx_mu_);
            for (auto& [sym, s] : context_) {
                if (s.mask == 0 && s.bbo.samples == 0) continue;
                auto ri = lookup_nolock(sym);
                if (!ri) continue;
                rows.push_back({sym, ri->instrument_id, s});
                s.bbo = BboMinute{};
            }
        }
        if (rows.empty()) return;
        persistence::with_txn_logged(pool_, "runtime_minute_roll", [&](pqxx::work& txn) {
            const auto dec = [&](const std::optional<Decimal>& d) {
                return d ? txn.quote(d->to_string()) + "::numeric" : std::string("NULL");
            };
            for (const auto& r : rows) {
                const auto& s = r.state;
                if (s.mask != 0) {
                    std::optional<Decimal> spread;
                    if (s.best_bid && s.best_ask) {
                        const Decimal neg{-s.best_bid->mantissa(), s.best_bid->scale()};
                        if (auto sp = s.best_ask->add(neg)) spread = *sp;
                    }
                    txn.exec(
                        "INSERT INTO market_derivatives_context_1m (exchange_id, instrument_id, "
                        "open_time, mark_price, index_price, funding_rate, next_funding_time, "
                        "open_interest_base, open_interest_quote, best_bid, best_ask, spread, "
                        "turnover_24h, source_mask) VALUES (" +
                        std::to_string(spec_.exchange_id) + "," + std::to_string(r.instrument_id) +
                        ",to_timestamp(" + std::to_string(minute_open_ms) +
                        "::double precision/1000.0)," + dec(s.mark) + "," + dec(s.index) + "," +
                        dec(s.funding) + "," +
                        (s.next_funding_ms > 0
                             ? "to_timestamp(" + std::to_string(s.next_funding_ms) +
                                   "::double precision/1000.0)"
                             : "NULL") +
                        "," + dec(s.oi_base) + "," + dec(s.oi_quote) + "," + dec(s.best_bid) +
                        "," + dec(s.best_ask) + "," + dec(spread) + "," + dec(s.turnover_24h) +
                        "," + std::to_string(s.mask) +
                        ") ON CONFLICT (exchange_id, instrument_id, open_time) DO UPDATE SET "
                        "mark_price = COALESCE(EXCLUDED.mark_price, "
                        "market_derivatives_context_1m.mark_price), "
                        "index_price = COALESCE(EXCLUDED.index_price, "
                        "market_derivatives_context_1m.index_price), "
                        "funding_rate = COALESCE(EXCLUDED.funding_rate, "
                        "market_derivatives_context_1m.funding_rate), "
                        "next_funding_time = COALESCE(EXCLUDED.next_funding_time, "
                        "market_derivatives_context_1m.next_funding_time), "
                        "open_interest_base = COALESCE(EXCLUDED.open_interest_base, "
                        "market_derivatives_context_1m.open_interest_base), "
                        "open_interest_quote = COALESCE(EXCLUDED.open_interest_quote, "
                        "market_derivatives_context_1m.open_interest_quote), "
                        "best_bid = COALESCE(EXCLUDED.best_bid, "
                        "market_derivatives_context_1m.best_bid), "
                        "best_ask = COALESCE(EXCLUDED.best_ask, "
                        "market_derivatives_context_1m.best_ask), "
                        "spread = COALESCE(EXCLUDED.spread, market_derivatives_context_1m.spread), "
                        "turnover_24h = COALESCE(EXCLUDED.turnover_24h, "
                        "market_derivatives_context_1m.turnover_24h), "
                        "source_mask = market_derivatives_context_1m.source_mask | "
                        "EXCLUDED.source_mask, updated_at = now()");
                }
                if (s.bbo.samples > 0) {
                    txn.exec(
                        "INSERT INTO market_bbo_1m (exchange_id, instrument_id, open_time, "
                        "bid_first, bid_last, bid_min, bid_max, ask_first, ask_last, ask_min, "
                        "ask_max, spread_min, spread_max, sample_count) VALUES (" +
                        std::to_string(spec_.exchange_id) + "," + std::to_string(r.instrument_id) +
                        ",to_timestamp(" + std::to_string(minute_open_ms) +
                        "::double precision/1000.0)," + dec(s.bbo.bid_first) + "," +
                        dec(s.bbo.bid_last) + "," + dec(s.bbo.bid_min) + "," +
                        dec(s.bbo.bid_max) + "," + dec(s.bbo.ask_first) + "," +
                        dec(s.bbo.ask_last) + "," + dec(s.bbo.ask_min) + "," +
                        dec(s.bbo.ask_max) + "," + dec(s.bbo.spread_min) + "," +
                        dec(s.bbo.spread_max) + "," + std::to_string(s.bbo.samples) +
                        ") ON CONFLICT (exchange_id, instrument_id, open_time) DO UPDATE SET "
                        "bid_last = EXCLUDED.bid_last, ask_last = EXCLUDED.ask_last, "
                        "sample_count = market_bbo_1m.sample_count + EXCLUDED.sample_count, "
                        "updated_at = now()");
                }
            }
        });
    }

    // Note: locks inst_mu_ (callers hold ctx_mu_ only; ctx_mu_ -> inst_mu_ is
    // the single lock ordering, so no deadlock).
    std::optional<RuntimeInstrument> lookup_nolock(const std::string& symbol) {
        std::lock_guard lk(inst_mu_);
        const auto it = instruments_.find(symbol);
        if (it == instruments_.end()) return std::nullopt;
        return it->second;
    }

    // ---- REST worker: book resync, OI polling, funding backfill -------------
    void rest_loop() {
        auto& shutdown = ShutdownToken::instance();
        int64_t last_oi_poll = 0;
        int64_t last_funding = 0;
        int64_t last_mark_index = 0;
        while (!shutdown.stop_requested()) {
            // Binance book resync via REST depth snapshot.
            std::string symbol;
            {
                std::lock_guard lk(resync_mu_);
                if (!resync_queue_.empty()) {
                    symbol = std::move(resync_queue_.front());
                    resync_queue_.pop_front();
                    resync_queued_.erase(symbol);
                }
            }
            if (!symbol.empty() && adapter_) {
                resync_binance_book(symbol);
                continue;
            }

            const int64_t now = timeutil::now_ms();
            // OI polling for venues without an OI stream (Binance).
            if (spec_.exchange_id == exchanges::kVenueBinanceUsdm && adapter_ &&
                now - last_oi_poll >= oi_poll_interval_sec_ * 1000) {
                last_oi_poll = now;
                poll_open_interest();
            }
            // Funding settlement history: hourly incremental sync (bounded).
            //
            // This runs for context-only venues too. `context_only` means "no
            // order book or candle ingestion here", not "no derivatives
            // context" — settlement history IS the derivatives context this
            // runtime exists to collect. Excluding it left Bitget, the primary
            // trading venue, as the only one with no settlement history at all,
            // so AltTech could not derive its funding interval and
            // `bitget_funding_deviation` was never computed. An adapter that
            // does not implement fetch_funding_history returns "not supported"
            // and the sync simply finds nothing.
            if (adapter_ && now - last_funding >= 3600'000) {
                if (sync_funding_settlements()) last_funding = now;
            }
            // Mark/index history. Unlike open interest — which Bitget does not
            // serve historically at all — both series are backfillable on every
            // venue, and AltTech's cross-venue consensus reads them
            // (consensus_mark_price, mark_deviation). Without this they exist
            // only from the moment live collection started, so any historical
            // window has them null.
            if (adapter_ && mark_index_backfill_days_ > 0 &&
                now - last_mark_index >= 3600'000) {
                if (sync_mark_index_history()) last_mark_index = now;
            }
            if (shutdown.wait_for(std::chrono::milliseconds(200))) break;
        }
    }

    void resync_binance_book(const std::string& symbol) {
        auto snap = adapter_->fetch_orderbook_snapshot(symbol, 0); // configured depth
        if (!snap) {
            log::warn("book_resync_failed",
                      {{"venue", spec_.code}, {"symbol", symbol},
                       {"message", snap.error().message}});
            enqueue_resync(symbol); // retried on the next delta / next pass
            return;
        }
        auto ri = lookup(symbol);
        if (!ri) return;
        BookState* st = book_state(symbol);
        bool seeded = false;
        {
            std::lock_guard lk(st->mu);
            st->book->reset_from_rest(std::move(snap->bids), std::move(snap->asks),
                                      snap->sequence, snap->exchange_ms);
            seeded = st->book->synced();
            st->desynced = !seeded;
        }
        if (!seeded) {
            log::warn("book_resync_invalid", {{"venue", spec_.code}, {"symbol", symbol}});
            enqueue_resync(symbol);
            return;
        }
        publish_integrity(symbol, *ri, "book_recovered", "REST snapshot applied");
        log::info("book_recovered", {{"venue", spec_.code}, {"symbol", symbol}});
    }

    void poll_open_interest() {
        std::vector<RuntimeInstrument> list;
        {
            std::lock_guard lk(inst_mu_);
            for (const auto& [_, ri] : instruments_) list.push_back(ri);
        }
        auto& shutdown = ShutdownToken::instance();
        for (const auto& ri : list) {
            if (shutdown.stop_requested()) return;
            auto oi = adapter_->fetch_open_interest(ri.symbol);
            if (!oi) continue;
            exchanges::VenueTickerEvent ev;
            ev.symbol = ri.symbol;
            ev.exchange_ms = oi->ts_ms;
            ev.open_interest_base = oi->base;
            ev.open_interest_quote = oi->quote;
            handle_ticker(ev, timeutil::now_ms(), "rest-poll");
        }
    }

    // Backfill mark and index price into the per-minute derivatives context.
    //
    // The rows written are honestly PARTIAL: source_mask carries bits 1|2 and
    // every other column stays NULL, because open interest, funding and BBO
    // were never observed for those minutes and no venue serves them
    // historically. That is what source_mask is for; nothing is carried over
    // from a neighbouring minute to make a row look complete.
    bool sync_mark_index_history() {
        std::vector<RuntimeInstrument> list;
        {
            std::lock_guard lk(inst_mu_);
            for (const auto& [_, ri] : instruments_) list.push_back(ri);
        }
        if (list.empty()) return false;

        auto& shutdown = ShutdownToken::instance();
        const int64_t now = timeutil::now_ms();
        const int64_t target_from = now - mark_index_backfill_days_ * 86'400'000LL;
        int64_t written = 0;
        int64_t instruments_filled = 0;

        // The partitions must exist before the first historical row arrives.
        // market_derivatives_context_1m is created with a fixed window at
        // migration time, so a backfill reaching further back writes into a
        // range that has no partition and every row is rejected -- the same
        // failure the candle backfill had, in a table the candle helper does
        // not cover.
        const bool partitions_ready = pool_.with_txn([&](pqxx::work& txn) {
            txn.exec_params(
                "SELECT altspotter_ensure_time_partitions("
                "  'market_derivatives_context_1m'::regclass, 'month', "
                "  to_timestamp($1::bigint / 1000.0), to_timestamp($2::bigint / 1000.0))",
                target_from, now + 31LL * 86'400'000);
        }).has_value();
        if (!partitions_ready) {
            log::error("mark_index_partitions_unavailable", {{"venue", spec_.code}});
            return false;
        }

        for (const auto& ri : list) {
            if (shutdown.stop_requested()) return true;

            // Only fetch what is missing below the earliest stored mark.
            int64_t oldest_marked = 0;
            auto wm = pool_.with_txn([&](pqxx::work& txn) {
                const auto r = txn.exec_params(
                    "SELECT COALESCE((extract(epoch FROM min(open_time)) * 1000)::bigint, 0) "
                    "FROM market_derivatives_context_1m "
                    "WHERE exchange_id = $1 AND instrument_id = $2 AND mark_price IS NOT NULL",
                    spec_.exchange_id, ri.instrument_id);
                oldest_marked = r[0][0].as<int64_t>();
            });
            if (!wm) continue;

            const int64_t to = oldest_marked > 0 ? oldest_marked : now;
            if (to - target_from < 60'000) continue; // already deep enough

            auto hist = adapter_->fetch_mark_index_history(ri.symbol, target_from, to);
            if (!hist) {
                if (hist.error().code != ErrorCode::invalid_argument) {
                    log::warn("mark_index_history_failed",
                              {{"venue", spec_.code},
                               {"symbol", ri.symbol},
                               {"message", hist.error().message}});
                }
                continue;
            }
            if (hist->empty()) continue;

            auto ok = pool_.with_txn([&](pqxx::work& txn) {
                txn.exec("SET LOCAL TIME ZONE 'UTC'");
                for (const auto& sample : *hist) {
                    if (!sample.mark_price && !sample.index_price) continue;
                    const int mask = (sample.mark_price ? 1 : 0) | (sample.index_price ? 2 : 0);
                    txn.exec_params(
                        "INSERT INTO market_derivatives_context_1m "
                        "(exchange_id, instrument_id, open_time, mark_price, index_price, "
                        " quality_status, source_mask) "
                        "VALUES ($1, $2, to_timestamp($3::bigint / 1000.0), "
                        "        $4::numeric, $5::numeric, 'ok', $6) "
                        "ON CONFLICT (exchange_id, instrument_id, open_time) DO UPDATE SET "
                        "  mark_price = COALESCE(market_derivatives_context_1m.mark_price, "
                        "                        EXCLUDED.mark_price), "
                        "  index_price = COALESCE(market_derivatives_context_1m.index_price, "
                        "                         EXCLUDED.index_price), "
                        "  source_mask = market_derivatives_context_1m.source_mask | "
                        "                EXCLUDED.source_mask, "
                        "  updated_at = now()",
                        spec_.exchange_id, ri.instrument_id, sample.open_time_ms,
                        // A missing series must be SQL NULL, not an empty string:
                        // "" would fail the numeric cast, and any placeholder
                        // value would be a fabricated observation.
                        sample.mark_price
                            ? std::optional<std::string>(sample.mark_price->to_string())
                            : std::nullopt,
                        sample.index_price
                            ? std::optional<std::string>(sample.index_price->to_string())
                            : std::nullopt,
                        mask);
                    ++written;
                }
            });
            if (!ok) {
                log::warn("mark_index_write_failed",
                          {{"venue", spec_.code}, {"symbol", ri.symbol},
                           {"message", ok.error().message}});
                continue;
            }
            ++instruments_filled;
        }

        if (written > 0) {
            log::info("mark_index_history_synced",
                      {{"venue", spec_.code},
                       {"instruments", instruments_filled},
                       {"rows", written}});
        }
        return true;
    }

    // Returns false when there was nothing to sync yet, so the caller does not
    // burn its hourly slot on an empty pass.
    bool sync_funding_settlements() {
        std::vector<RuntimeInstrument> list;
        {
            std::lock_guard lk(inst_mu_);
            for (const auto& [_, ri] : instruments_) list.push_back(ri);
        }
        // The REST loop starts before the first instrument reload completes, so
        // its first tick found an empty universe, reported success and then slept
        // for an hour. Every venue was affected; the reference venues only ever
        // caught up on a later hourly tick, and Bitget — whose adapter was
        // missing entirely — never did.
        if (list.empty()) return false;
        auto& shutdown = ShutdownToken::instance();
        const int64_t now = timeutil::now_ms();
        // The sync used to leave no trace at all, so a venue silently collecting
        // nothing looked identical to one that was up to date.
        int64_t written = 0;
        int64_t skipped_recent = 0;
        int64_t empty_responses = 0;
        int64_t older_unavailable = 0;
        for (const auto& ri : list) {
            // Shutting down mid-pass: what was written stays, and the slot is
            // treated as used so a stopping process does not re-enter the loop.
            if (shutdown.stop_requested()) return true;
            int64_t newest = 0;
            int64_t oldest = 0;
            auto wm = pool_.with_txn([&](pqxx::work& txn) {
                const auto r = txn.exec_params(
                    "SELECT COALESCE((extract(epoch FROM max(funding_time)) * 1000)::bigint, 0), "
                    "       COALESCE((extract(epoch FROM min(funding_time)) * 1000)::bigint, 0) "
                    "FROM market_funding_settlements WHERE exchange_id = $1 AND instrument_id = $2",
                    spec_.exchange_id, ri.instrument_id);
                newest = r[0][0].as<int64_t>();
                oldest = r[0][1].as<int64_t>();
            });
            if (!wm) continue;

            const int64_t target_from = now - funding_backfill_days_ * 86'400'000LL;
            // New settlements first, and independently of the deepening: see
            // funding_sync.hpp for the venue whose short history used to block
            // them for good.
            const auto plan = funding::plan_fetches(
                newest, oldest, now, target_from,
                funding_older_exhausted_.count(ri.instrument_id) > 0);
            if (plan.empty()) { ++skipped_recent; continue; }
            for (const auto& range : plan) {
                if (shutdown.stop_requested()) return true;
                auto hist = adapter_->fetch_funding_history(ri.symbol, range.from_ms + 1,
                                                            range.to_ms);
                if (!hist) {
                    // Never silent: a venue whose settlement history stops
                    // arriving loses its funding interval downstream, and the
                    // only symptom is an analytics field that quietly stays unset.
                    log::warn("funding_history_fetch_failed",
                              {{"venue", spec_.code},
                               {"symbol", ri.symbol},
                               {"deepen", range.deepen},
                               {"message", hist.error().message}});
                    continue;
                }
                if (hist->empty()) {
                    if (range.deepen) {
                        // The venue has nothing older: its history ends here.
                        funding_older_exhausted_.insert(ri.instrument_id);
                        ++older_unavailable;
                    } else {
                        ++empty_responses;
                    }
                    continue;
                }
                written += static_cast<int64_t>(hist->size());
                persistence::with_txn_logged(pool_, "funding_settlements_insert",
                                             [&](pqxx::work& txn) {
                    for (const auto& f : *hist) {
                        txn.exec_params(
                            "INSERT INTO market_funding_settlements (exchange_id, instrument_id, "
                            "funding_time, funding_rate, mark_price) "
                            "VALUES ($1, $2, to_timestamp($3::double precision/1000.0), $4, $5) "
                            "ON CONFLICT DO NOTHING",
                            spec_.exchange_id, ri.instrument_id, f.funding_time_ms,
                            f.rate.to_string(),
                            f.mark_price ? std::optional<std::string>(f.mark_price->to_string())
                                         : std::nullopt);
                    }
                });
            }
        }
        log::info("funding_settlements_synced",
                  {{"venue", spec_.code},
                   {"instruments", static_cast<int64_t>(list.size())},
                   {"settlements_fetched", written},
                   {"up_to_date", skipped_recent},
                   {"empty_responses", empty_responses},
                   {"older_unavailable", older_unavailable}});
        return true;
    }

    // ---- operational state --------------------------------------------------
    void update_ops_state() {
        const bool conn = session_ && session_->connected();
        const bool conn2 = !session2_ || session2_->connected();
        const std::string state = !sessions_started_ ? "STARTING"
                                  : (conn && conn2)  ? "HEALTHY"
                                                     : "RECONNECTING";
        std::vector<std::tuple<std::string, int64_t, ContextState>> quality;
        {
            std::lock_guard lk(ctx_mu_);
            for (const auto& [sym, s] : context_) {
                auto ri = lookup_nolock(sym);
                if (ri) quality.emplace_back(sym, ri->instrument_id, s);
            }
        }
        const int64_t now = timeutil::now_ms();
        persistence::with_txn_logged(pool_, "runtime_ops_state", [&](pqxx::work& txn) {
            txn.exec_params(
                "INSERT INTO exchange_connections (exchange_id, connection_id, state, "
                "reconnect_count, connected_at, updated_at) "
                "VALUES ($1, $2, $3, $4, CASE WHEN $3 = 'HEALTHY' THEN now() END, now()) "
                "ON CONFLICT (exchange_id, connection_id) DO UPDATE SET state = $3, "
                "reconnect_count = $4, "
                "connected_at = CASE WHEN $3 = 'HEALTHY' AND exchange_connections.state <> "
                "'HEALTHY' THEN now() ELSE exchange_connections.connected_at END, "
                "disconnected_at = CASE WHEN $3 <> 'HEALTHY' AND exchange_connections.state = "
                "'HEALTHY' THEN now() ELSE exchange_connections.disconnected_at END, "
                "updated_at = now()",
                spec_.exchange_id, std::string(spec_.code) + "-ws", state,
                reconnects_.load(std::memory_order_relaxed));
            txn.exec_params(
                "INSERT INTO exchange_clock_offsets (exchange_id, offset_ms, sample_count, "
                "updated_at) VALUES ($1, $2, $3, now()) "
                "ON CONFLICT (exchange_id) DO UPDATE SET offset_ms = $2, sample_count = $3, "
                "updated_at = now()",
                spec_.exchange_id, clock_offset_ms_.load(std::memory_order_relaxed),
                clock_samples_.load(std::memory_order_relaxed));
            for (const auto& [sym, iid, s] : quality) {
                const auto age = [&](int64_t seen) -> std::optional<int64_t> {
                    if (seen <= 0) return std::nullopt;
                    return (now - seen) / 1000;
                };
                std::string q = "VALID";
                const auto book_age = age(s.seen_book);
                const auto trade_age = age(s.seen_trade);
                if (!conn) {
                    q = "DEGRADED";
                } else if ((book_age && *book_age > 120) || (trade_age && *trade_age > 600)) {
                    q = "STALE";
                }
                txn.exec_params(
                    "INSERT INTO exchange_data_quality (exchange_id, instrument_id, "
                    "trade_age_sec, candle_age_sec, book_age_sec, ticker_age_sec, mark_age_sec, "
                    "oi_age_sec, sequence_gap_count, reconnect_count, crossed_book_count, "
                    "quality, updated_at) "
                    "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12, now()) "
                    "ON CONFLICT (exchange_id, instrument_id) DO UPDATE SET "
                    "trade_age_sec = $3, candle_age_sec = $4, book_age_sec = $5, "
                    "ticker_age_sec = $6, mark_age_sec = $7, oi_age_sec = $8, "
                    "sequence_gap_count = $9, reconnect_count = $10, crossed_book_count = $11, "
                    "quality = $12, updated_at = now()",
                    spec_.exchange_id, iid, age(s.seen_trade), age(s.seen_candle),
                    age(s.seen_book), age(s.seen_ticker), age(s.seen_mark), age(s.seen_oi),
                    s.seq_gaps, reconnects_.load(std::memory_order_relaxed), s.crossed, q);
            }
        });
    }

    // ---- members ------------------------------------------------------------
    config::ServiceContext ctx_;
    VenueSpec spec_;
    persistence::PgPool pool_;
    std::unique_ptr<messaging::NatsClient> nats_;
    std::unique_ptr<messaging::JsPublishQueue> js_;
    std::unique_ptr<exchanges::ExchangeMarketDataAdapter> adapter_;
    exchanges::binance::BinanceConfig binance_cfg_;
    exchanges::bybit::BybitConfig bybit_cfg_;
    exchanges::okx::OkxConfig okx_cfg_;

    std::unique_ptr<exchanges::VenueWsSession> session_;
    std::unique_ptr<exchanges::VenueWsSession> session2_;
    bool sessions_started_ = false;

    std::mutex inst_mu_;
    std::map<std::string, RuntimeInstrument> instruments_;
    std::vector<std::string> known_symbols_;

    std::mutex book_mu_;
    std::map<std::string, std::unique_ptr<BookState>> books_;

    std::mutex ctx_mu_;
    std::map<std::string, ContextState> context_;

    std::mutex write_mu_;
    std::condition_variable write_cv_;
    std::vector<PendingTrade> pending_trades_;
    std::vector<PendingSnapshot> pending_snapshots_;
    bool write_stop_ = false;
    std::thread writer_thread_;
    std::thread rest_thread_;

    std::mutex resync_mu_;
    std::deque<std::string> resync_queue_;
    std::set<std::string> resync_queued_; // guards against duplicate enqueues

    std::atomic<int64_t> clock_offset_ms_{0};
    std::atomic<int64_t> clock_samples_{0};
    std::atomic<int64_t> reconnects_{0};
    std::atomic<int64_t> bars_published_{0};
    // Due book samples dropped before storage by the persistence gate.
    std::atomic<int64_t> books_shed_{0};
    std::atomic<int64_t> last_shed_log_ms_{0};

    int64_t core_interval_ms_ = 5000;
    int64_t other_interval_ms_ = 15000;
    int snapshot_depth_ = 15;
    int64_t bbo_pub_interval_ms_ = 1000;
    int64_t ticker_pub_interval_ms_ = 1000;
    int64_t context_pub_interval_ms_ = 3000;
    int64_t flush_interval_ms_ = 1000;
    int64_t reload_interval_sec_ = 120;
    int64_t oi_poll_interval_sec_ = 60;
    int64_t funding_backfill_days_ = 180;
    // Instruments whose venue answered the deepening request with nothing:
    // its history ends there. Not asked again for the life of this process.
    std::set<int64_t> funding_older_exhausted_;
    int64_t mark_index_backfill_days_ = 0;
    size_t max_pending_rows_ = 200000;
};

int run_venue(config::ServiceContext ctx, VenueSpec spec) {
    VenueRuntime runtime(std::move(ctx), spec);
    return runtime.run();
}

} // namespace

namespace altspotter::components {

int binance_usdm_runtime(config::ServiceContext ctx) {
    return run_venue(std::move(ctx),
                     {exchanges::kVenueBinanceUsdm, "binance_usdm", "binance_usdm_runtime", false,
                      exchanges::LocalBook::Sequencing::binance_futures});
}

int bybit_runtime(config::ServiceContext ctx) {
    return run_venue(std::move(ctx), {exchanges::kVenueBybit, "bybit", "bybit_runtime",
                                      false, exchanges::LocalBook::Sequencing::bybit});
}

int okx_runtime(config::ServiceContext ctx) {
    return run_venue(std::move(ctx), {exchanges::kVenueOkx, "okx", "okx_runtime", false,
                                      exchanges::LocalBook::Sequencing::okx});
}

int bitget_context(config::ServiceContext ctx) {
    return run_venue(std::move(ctx), {exchanges::kVenueBitget, "bitget", "bitget_context_runtime",
                                      true, exchanges::LocalBook::Sequencing::bybit});
}

} // namespace altspotter::components
