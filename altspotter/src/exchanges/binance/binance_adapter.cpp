#include "exchanges/pagination.hpp"
#include <map>
#include "exchanges/binance/binance_adapter.hpp"

#include "core/time.hpp"
#include "logging/logger.hpp"
#include "exchanges/adapter_util.hpp"

#include <algorithm>
#include <cctype>

namespace altspotter::exchanges::binance {

using json = nlohmann::json;

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

Result<Decimal> dec_str(const json& v, const char* what) {
    if (v.is_string()) return Decimal::parse(v.get_ref<const std::string&>());
    return fail(ErrorCode::parse_error, std::string("expected decimal string: ") + what);
}

std::optional<Decimal> opt_dec(const json& obj, const char* key) {
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) return std::nullopt;
    auto d = Decimal::parse(it->get_ref<const std::string&>());
    if (!d) return std::nullopt;
    return *d;
}

Result<std::vector<Level>> parse_levels(const json& arr, const char* what) {
    std::vector<Level> out;
    if (!arr.is_array()) return fail(ErrorCode::parse_error, std::string("levels not array: ") + what);
    out.reserve(arr.size());
    for (const auto& lvl : arr) {
        if (!lvl.is_array() || lvl.size() < 2 || !lvl[0].is_string() || !lvl[1].is_string()) {
            continue;
        }
        Level l;
        l.price_str = lvl[0].get<std::string>();
        l.size_str = lvl[1].get<std::string>();
        auto p = Decimal::parse(l.price_str);
        auto s = Decimal::parse(l.size_str);
        if (!p || !s) continue;
        l.price = *p;
        l.size = *s;
        out.push_back(std::move(l));
    }
    return out;
}

} // namespace

BinanceConfig parse_config(const YAML::Node& node) {
    BinanceConfig c;
    if (node && node.IsMap()) {
        c.rest_base_url = config::opt_str(node, "rest_base_url", c.rest_base_url);
        c.ws_url = config::opt_str(node, "ws_url", c.ws_url);
        c.ws_public_url = config::opt_str(node, "ws_public_url", c.ws_public_url);
        c.connect_timeout_ms = static_cast<int32_t>(
            config::opt_int(node, "connect_timeout_ms", c.connect_timeout_ms));
        c.request_timeout_ms = static_cast<int32_t>(
            config::opt_int(node, "request_timeout_ms", c.request_timeout_ms));
        c.rate_limit_per_second =
            config::opt_double(node, "rate_limit_per_second", c.rate_limit_per_second);
        c.max_retries = static_cast<int32_t>(config::opt_int(node, "max_retries", c.max_retries));
        c.backoff_min_ms =
            static_cast<int32_t>(config::opt_int(node, "backoff_min_ms", c.backoff_min_ms));
        c.backoff_max_ms =
            static_cast<int32_t>(config::opt_int(node, "backoff_max_ms", c.backoff_max_ms));
        c.stale_timeout_sec = static_cast<int32_t>(
            config::opt_int(node, "stale_timeout_sec", c.stale_timeout_sec));
        c.depth_snapshot_limit = static_cast<int32_t>(
            config::opt_int(node, "depth_snapshot_limit", c.depth_snapshot_limit));
    }
    return c;
}

BinanceUsdmMarketDataAdapter::BinanceUsdmMarketDataAdapter(BinanceConfig cfg,
                                                           std::function<bool()> should_stop)
    : cfg_(std::move(cfg)), should_stop_(std::move(should_stop)),
      client_(RestClientOptions{
          .host = host_from_url(cfg_.rest_base_url),
          .port = "443",
          .connect_timeout_ms = cfg_.connect_timeout_ms,
          .request_timeout_ms = cfg_.request_timeout_ms,
          .user_agent = "altspotter/0.2",
      }),
      bucket_(cfg_.rate_limit_per_second, std::max(2.0, cfg_.rate_limit_per_second)) {}

Result<json> BinanceUsdmMarketDataAdapter::get_json(const std::string& target) {
    auto res = get_with_retry(client_, bucket_, target, cfg_.max_retries, cfg_.backoff_min_ms,
                              cfg_.backoff_max_ms, should_stop_);
    if (!res) return std::unexpected(res.error());
    if (res->status != 200) return std::unexpected(map_exchange_error(res->status, res->body));
    json doc = json::parse(res->body, nullptr, false);
    if (doc.is_discarded()) return fail(ErrorCode::parse_error, "invalid JSON from " + target);
    return doc;
}

Result<std::vector<InstrumentSpec>> BinanceUsdmMarketDataAdapter::parse_exchange_info(
    const json& doc) {
    if (!doc.is_object() || !doc.contains("symbols") || !doc["symbols"].is_array()) {
        return fail(ErrorCode::parse_error, "exchangeInfo has no symbols array");
    }
    std::vector<InstrumentSpec> out;
    for (const auto& s : doc["symbols"]) {
        if (!s.is_object()) continue;
        if (s.value("contractType", "") != "PERPETUAL") continue;
        if (s.value("quoteAsset", "") != "USDT") continue;
        if (s.value("marginAsset", "") != "USDT") continue;

        InstrumentSpec spec;
        spec.exchange_symbol = s.value("symbol", "");
        spec.base_asset = s.value("baseAsset", "");
        spec.quote_asset = "USDT";
        spec.settle_asset = "USDT";
        spec.contract_type = "perpetual";
        const std::string status = s.value("status", "");
        if (status == "TRADING") {
            spec.status = InstrumentStatus::active;
        } else if (status == "SETTLING" || status == "PENDING_TRADING" || status == "BREAK" ||
                   status == "AUCTION_MATCH" || status == "PRE_SETTLE") {
            spec.status = InstrumentStatus::suspended;
        } else if (status == "CLOSE" || status == "DELIVERING" || status == "DELIVERED") {
            spec.status = InstrumentStatus::delisted;
        } else {
            spec.status = InstrumentStatus::suspended;
        }
        spec.list_time_ms = opt_ll(s, "onboardDate");
        spec.price_precision = static_cast<int32_t>(opt_ll(s, "pricePrecision"));
        spec.quantity_precision = static_cast<int32_t>(opt_ll(s, "quantityPrecision"));
        if (s.contains("filters") && s["filters"].is_array()) {
            for (const auto& f : s["filters"]) {
                const std::string type = f.value("filterType", "");
                if (type == "PRICE_FILTER") {
                    if (auto d = opt_dec(f, "tickSize")) spec.tick_size = *d;
                } else if (type == "LOT_SIZE") {
                    if (auto d = opt_dec(f, "stepSize")) spec.quantity_step = *d;
                    if (auto d = opt_dec(f, "minQty")) spec.min_order_quantity = *d;
                } else if (type == "MIN_NOTIONAL") {
                    if (auto d = opt_dec(f, "notional")) spec.min_notional = *d;
                }
            }
        }
        if (spec.exchange_symbol.empty() || spec.base_asset.empty()) continue;
        out.push_back(std::move(spec));
    }
    return out;
}

Result<std::vector<InstrumentSpec>> BinanceUsdmMarketDataAdapter::list_instruments() {
    auto doc = get_json("/fapi/v1/exchangeInfo");
    if (!doc) return std::unexpected(doc.error());
    return parse_exchange_info(*doc);
}

Result<InstrumentSpec> BinanceUsdmMarketDataAdapter::get_instrument_spec(
    const std::string& exchange_symbol) {
    auto all = list_instruments();
    if (!all) return std::unexpected(all.error());
    for (auto& s : *all) {
        if (s.exchange_symbol == exchange_symbol) return std::move(s);
    }
    return fail(ErrorCode::not_found, "contract not found: " + exchange_symbol);
}

Result<std::vector<Candle>> BinanceUsdmMarketDataAdapter::parse_klines(const json& payload,
                                                                       Timeframe tf,
                                                                       CandleSource source,
                                                                       int64_t now_ms) {
    if (!payload.is_array()) return fail(ErrorCode::parse_error, "klines payload is not an array");
    std::vector<Candle> out;
    out.reserve(payload.size());
    const int64_t span = tf_ms(tf);
    for (const auto& row : payload) {
        // [openTime, o, h, l, c, baseVol, closeTime, quoteVol, trades,
        //  takerBuyBase, takerBuyQuote, ignore]
        if (!row.is_array() || row.size() < 11) {
            return fail(ErrorCode::parse_error, "kline row has unexpected shape");
        }
        Candle c;
        c.timeframe = tf;
        if (!row[0].is_number_integer()) {
            return fail(ErrorCode::parse_error, "kline openTime is not an integer");
        }
        c.open_time_ms = row[0].get<int64_t>();
        c.close_time_ms = c.open_time_ms + span;
        auto o = dec_str(row[1], "open"), h = dec_str(row[2], "high"), l = dec_str(row[3], "low"),
             cl = dec_str(row[4], "close"), bv = dec_str(row[5], "baseVolume"),
             qv = dec_str(row[7], "quoteVolume"), tb = dec_str(row[9], "takerBuyBase");
        for (const auto* r : {&o, &h, &l, &cl, &bv, &qv, &tb}) {
            if (!*r) return std::unexpected(r->error());
        }
        c.open = *o;
        c.high = *h;
        c.low = *l;
        c.close = *cl;
        c.base_volume = *bv;
        c.quote_volume = *qv;
        if (row[8].is_number_integer()) c.trade_count = row[8].get<int64_t>();
        c.taker_buy_base_volume = *tb;
        // taker sell = base - taker buy (exact decimal arithmetic).
        const Decimal neg_buy{-tb->mantissa(), tb->scale()};
        if (auto sell = bv->add(neg_buy); sell && !sell->is_negative()) {
            c.taker_sell_base_volume = *sell;
        }
        c.is_closed = c.close_time_ms <= now_ms;
        c.source = source;
        c.quality = Quality::ok;
        if (!c.valid()) {
            return fail(ErrorCode::validation_error,
                        "invalid kline at open_time " + std::to_string(c.open_time_ms));
        }
        out.push_back(std::move(c));
    }
    std::sort(out.begin(), out.end(),
              [](const Candle& a, const Candle& b) { return a.open_time_ms < b.open_time_ms; });
    return out;
}

Result<CandlePage> BinanceUsdmMarketDataAdapter::fetch_candles(const std::string& exchange_symbol,
                                                               Timeframe tf, int64_t start_ms,
                                                               int64_t end_ms,
                                                               CandleQueryKind kind) {
    (void)kind; // one endpoint serves both recent and deep history
    const int64_t span = tf_ms(tf);
    // Walk contract: return the latest page inside [start_ms, end_ms).
    int64_t window_start = start_ms;
    if (end_ms > 0) {
        const int64_t page_span = static_cast<int64_t>(kKlinePageLimit) * span;
        if (end_ms - window_start > page_span) window_start = end_ms - page_span;
    }
    std::string target = "/fapi/v1/klines?symbol=" + exchange_symbol +
                         "&interval=1m&limit=" + std::to_string(kKlinePageLimit);
    if (window_start > 0) target += "&startTime=" + std::to_string(window_start);
    if (end_ms > 0) target += "&endTime=" + std::to_string(end_ms - 1);

    auto doc = get_json(target);
    if (!doc) return std::unexpected(doc.error());
    const auto source = kind == CandleQueryKind::historical ? CandleSource::rest_bootstrap
                                                            : CandleSource::rest_reconcile;
    auto candles = parse_klines(*doc, tf, source, timeutil::now_ms());
    if (!candles) return std::unexpected(candles.error());

    CandlePage page;
    page.exhausted = candles->empty();
    for (auto& c : *candles) {
        if (!c.is_closed) continue;
        if (start_ms > 0 && c.open_time_ms < start_ms) continue;
        if (end_ms > 0 && c.open_time_ms >= end_ms) continue;
        page.candles.push_back(std::move(c));
    }
    return page;
}


// Historical mark and index prices. Binance serves each as its own kline
// endpoint: markPriceKlines takes `symbol`, indexPriceKlines takes `pair`.
Result<std::vector<MarkIndexSample>> BinanceUsdmMarketDataAdapter::fetch_mark_index_history(
    const std::string& exchange_symbol, int64_t from_ms, int64_t to_ms) {
    const auto series = [&](const char* path,
                            const char* key) -> Result<std::map<int64_t, Decimal>> {
        std::map<int64_t, Decimal> out;
        int64_t cursor_end = to_ms;
        const int64_t span = tf_ms(Timeframe::m1);
        for (int page = 0; page < kMarkIndexMaxPages; ++page) {
            if (should_stop_ && should_stop_()) break;
            int64_t window_start = from_ms;
            const int64_t page_span = static_cast<int64_t>(kKlinePageLimit) * span;
            if (cursor_end > 0 && cursor_end - window_start > page_span) {
                window_start = cursor_end - page_span;
            }
            std::string target = std::string(path) + "?" + key + "=" + exchange_symbol +
                                 "&interval=1m&limit=" + std::to_string(kKlinePageLimit);
            if (window_start > 0) target += "&startTime=" + std::to_string(window_start);
            if (cursor_end > 0) target += "&endTime=" + std::to_string(cursor_end - 1);

            auto doc = get_json(target);
            if (!doc) {
                if (!out.empty()) break;
                return std::unexpected(doc.error());
            }
            if (!doc->is_array() || doc->empty()) break;

            int64_t oldest = 0;
            for (const auto& row : *doc) {
                if (!row.is_array() || row.size() < 5) continue;
                if (!row[0].is_number_integer()) continue;
                const int64_t ts = row[0].get<int64_t>();
                auto close = Decimal::parse(row[4].is_string() ? row[4].get<std::string>() : "");
                if (!close || ts <= 0) continue;
                if (from_ms > 0 && ts < from_ms) continue;
                if (to_ms > 0 && ts >= to_ms) continue;
                if (oldest == 0 || ts < oldest) oldest = ts;
                out.emplace(ts, *close);
            }
            if (oldest <= 0) break;
            auto next = exchanges::advance_history_cursor(oldest, cursor_end);
            if (!next) break;
            cursor_end = *next;
            if (exchanges::history_walk_done(oldest, from_ms)) break;
        }
        return out;
    };

    auto mark = series("/fapi/v1/markPriceKlines", "symbol");
    if (!mark) return std::unexpected(mark.error());
    auto index = series("/fapi/v1/indexPriceKlines", "pair");
    if (!index) return std::unexpected(index.error());

    std::map<int64_t, MarkIndexSample> merged;
    for (const auto& [ts, v] : *mark) merged[ts].mark_price = v;
    for (const auto& [ts, v] : *index) merged[ts].index_price = v;
    std::vector<MarkIndexSample> out;
    out.reserve(merged.size());
    for (auto& [ts, sample] : merged) {
        sample.open_time_ms = ts;
        out.push_back(std::move(sample));
    }
    return out;
}

Result<std::vector<FundingSettlement>> BinanceUsdmMarketDataAdapter::fetch_funding_history(
    const std::string& exchange_symbol, int64_t from_ms, int64_t to_ms) {
    std::string target = "/fapi/v1/fundingRate?symbol=" + exchange_symbol + "&limit=1000";
    if (from_ms > 0) target += "&startTime=" + std::to_string(from_ms);
    if (to_ms > 0) target += "&endTime=" + std::to_string(to_ms);
    auto doc = get_json(target);
    if (!doc) return std::unexpected(doc.error());
    if (!doc->is_array()) return fail(ErrorCode::parse_error, "fundingRate payload not array");
    std::vector<FundingSettlement> out;
    for (const auto& r : *doc) {
        FundingSettlement f;
        f.funding_time_ms = opt_ll(r, "fundingTime");
        auto rate = opt_dec(r, "fundingRate");
        if (!rate || f.funding_time_ms <= 0) continue;
        f.rate = *rate;
        f.mark_price = opt_dec(r, "markPrice");
        out.push_back(std::move(f));
    }
    std::sort(out.begin(), out.end(), [](const FundingSettlement& a, const FundingSettlement& b) {
        return a.funding_time_ms < b.funding_time_ms;
    });
    return out;
}

Result<OpenInterestSample> BinanceUsdmMarketDataAdapter::fetch_open_interest(
    const std::string& exchange_symbol) {
    auto doc = get_json("/fapi/v1/openInterest?symbol=" + exchange_symbol);
    if (!doc) return std::unexpected(doc.error());
    OpenInterestSample s;
    s.ts_ms = opt_ll(*doc, "time");
    s.base = opt_dec(*doc, "openInterest");
    if (!s.base) return fail(ErrorCode::parse_error, "openInterest missing");
    return s;
}

Result<BookUpdate> BinanceUsdmMarketDataAdapter::fetch_orderbook_snapshot(
    const std::string& exchange_symbol, int depth) {
    const int limit = depth > 0 ? depth : cfg_.depth_snapshot_limit;
    auto doc = get_json("/fapi/v1/depth?symbol=" + exchange_symbol +
                        "&limit=" + std::to_string(limit));
    if (!doc) return std::unexpected(doc.error());
    BookUpdate u;
    u.is_snapshot = true;
    u.sequence = opt_ll(*doc, "lastUpdateId");
    u.exchange_ms = opt_ll(*doc, "E");
    auto bids = parse_levels((*doc)["bids"], "bids");
    auto asks = parse_levels((*doc)["asks"], "asks");
    if (!bids || !asks) return fail(ErrorCode::parse_error, "depth snapshot levels invalid");
    u.bids = std::move(*bids);
    u.asks = std::move(*asks);
    return u;
}

Error BinanceUsdmMarketDataAdapter::map_exchange_error(int http_status,
                                                       const std::string& body) const {
    int64_t code = 0;
    std::string msg;
    json doc = json::parse(body, nullptr, false);
    if (!doc.is_discarded() && doc.is_object()) {
        if (doc.contains("code") && doc["code"].is_number_integer()) code = doc["code"].get<int64_t>();
        if (doc.contains("msg") && doc["msg"].is_string()) msg = doc["msg"];
    }
    const std::string detail = "binance error http=" + std::to_string(http_status) +
                               " code=" + std::to_string(code) + " msg=" + msg.substr(0, 200);
    if (http_status == 429 || http_status == 418 || code == -1003) {
        return Error{ErrorCode::rate_limited, detail, true};
    }
    if (http_status >= 500) return Error{ErrorCode::upstream_error, detail, true};
    if (code == -1121) return Error{ErrorCode::not_found, detail, false}; // invalid symbol
    if (http_status == 400 || http_status == 404) {
        return Error{ErrorCode::invalid_argument, detail, false};
    }
    return Error{ErrorCode::upstream_error, detail, false};
}

// ---------------------------------------------------------------------------
// WebSocket protocol
// ---------------------------------------------------------------------------

std::string BinanceWsProtocol::ws_url() const {
    return group_ == StreamGroup::depth ? cfg_.ws_public_url : cfg_.ws_url;
}

std::vector<std::string> BinanceWsProtocol::subscribe_frames(
    const std::vector<std::string>& symbols) const {
    // One SUBSCRIBE per batch of streams (respect the 10 msgs/sec inbound cap).
    std::vector<std::string> frames;
    json params = json::array();
    const auto flush = [&] {
        if (params.empty()) return;
        frames.push_back(json{{"method", "SUBSCRIBE"}, {"params", params}, {"id", ++sub_id_}}.dump());
        params = json::array();
    };
    for (const auto& sym : symbols) {
        const std::string s = lower(sym);
        if (group_ == StreamGroup::depth) {
            params.push_back(s + "@depth");      // diff depth, 250ms default speed
        } else {
            params.push_back(s + "@aggTrade");
            params.push_back(s + "@kline_1m");
            params.push_back(s + "@markPrice");  // mark/index/funding, 3s
            params.push_back(s + "@ticker");     // 24h rolling stats
        }
        if (params.size() >= 40) flush();
    }
    flush();
    return frames;
}

bool BinanceWsProtocol::parse(std::string_view frame, VenueEvents& out) {
    json doc = json::parse(frame, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) return false;

    // SUBSCRIBE acks: {"result":null,"id":N}; errors: {"error":{...},"id":N}
    if (doc.contains("result") || doc.contains("error")) {
        if (doc.contains("error")) {
            out.protocol_error = doc["error"].dump();
        }
        return true;
    }
    const json& data = doc.contains("data") ? doc["data"] : doc; // combined vs raw
    if (!data.is_object()) return true;
    const std::string event = data.value("e", "");
    const std::string symbol = data.value("s", "");
    if (event.empty() || symbol.empty()) return true;

    if (event == "aggTrade") {
        VenueTradeEvent t;
        t.symbol = symbol;
        t.trade.exchange_trade_id = "a" + std::to_string(opt_ll(data, "a"));
        auto p = opt_dec(data, "p");
        auto q = opt_dec(data, "q");
        if (!p || !q) return true;
        t.trade.price = *p;
        t.trade.size = *q;
        // m == true => buyer is the maker => the taker (aggressor) sold.
        const bool buyer_is_maker = data.value("m", false);
        t.trade.side = buyer_is_maker ? TradeSide::sell : TradeSide::buy;
        t.trade.exchange_ts_ms = opt_ll(data, "T");
        out.trades.push_back(std::move(t));
        return true;
    }
    if (event == "kline") {
        const auto it = data.find("k");
        if (it == data.end() || !it->is_object()) return true;
        const json& k = *it;
        VenueCandleEvent c;
        c.symbol = symbol;
        c.candle.timeframe = Timeframe::m1;
        c.candle.open_time_ms = opt_ll(k, "t");
        c.candle.close_time_ms = c.candle.open_time_ms + timeutil::kMsPerMinute;
        auto o = opt_dec(k, "o"), h = opt_dec(k, "h"), l = opt_dec(k, "l"), cl = opt_dec(k, "c"),
             v = opt_dec(k, "v"), q = opt_dec(k, "q"), tb = opt_dec(k, "V");
        if (!o || !h || !l || !cl || !v || !q) return true;
        c.candle.open = *o;
        c.candle.high = *h;
        c.candle.low = *l;
        c.candle.close = *cl;
        c.candle.base_volume = *v;
        c.candle.quote_volume = *q;
        c.candle.trade_count = opt_ll(k, "n");
        if (tb) {
            c.candle.taker_buy_base_volume = *tb;
            const Decimal neg{-tb->mantissa(), tb->scale()};
            if (auto sell = v->add(neg); sell && !sell->is_negative()) {
                c.candle.taker_sell_base_volume = *sell;
            }
        }
        c.candle.is_closed = k.value("x", false);
        c.candle.source = CandleSource::ws;
        c.candle.quality = Quality::ok;
        out.candles.push_back(std::move(c));
        return true;
    }
    if (event == "depthUpdate") {
        VenueBookEvent b;
        b.symbol = symbol;
        b.update.is_snapshot = false;
        b.update.first_update_id = opt_ll(data, "U");
        b.update.sequence = opt_ll(data, "u");
        b.update.prev_sequence = data.contains("pu") ? opt_ll(data, "pu") : -1;
        b.update.exchange_ms = opt_ll(data, "T") > 0 ? opt_ll(data, "T") : opt_ll(data, "E");
        auto bids = parse_levels(data.value("b", json::array()), "b");
        auto asks = parse_levels(data.value("a", json::array()), "a");
        if (!bids || !asks) return true;
        b.update.bids = std::move(*bids);
        b.update.asks = std::move(*asks);
        out.books.push_back(std::move(b));
        return true;
    }
    if (event == "markPriceUpdate") {
        VenueTickerEvent t;
        t.symbol = symbol;
        t.exchange_ms = opt_ll(data, "E");
        t.mark_price = opt_dec(data, "p");
        t.index_price = opt_dec(data, "i");
        t.funding_rate = opt_dec(data, "r");
        t.next_funding_time_ms = opt_ll(data, "T");
        out.tickers.push_back(std::move(t));
        return true;
    }
    if (event == "24hrTicker") {
        VenueTickerEvent t;
        t.symbol = symbol;
        t.exchange_ms = opt_ll(data, "E");
        t.last_price = opt_dec(data, "c");
        t.open_24h = opt_dec(data, "o");
        t.high_24h = opt_dec(data, "h");
        t.low_24h = opt_dec(data, "l");
        t.base_volume_24h = opt_dec(data, "v");
        t.quote_volume_24h = opt_dec(data, "q");
        out.tickers.push_back(std::move(t));
        return true;
    }
    return true; // recognized JSON, uninteresting event
}

} // namespace altspotter::exchanges::binance
