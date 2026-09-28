#include <cstdlib>
#include <map>
#include "exchanges/pagination.hpp"
#include "exchanges/bybit/bybit_adapter.hpp"

#include "core/time.hpp"
#include "logging/logger.hpp"
#include "exchanges/adapter_util.hpp"

#include <algorithm>

namespace altspotter::exchanges::bybit {

using json = nlohmann::json;

namespace {

std::optional<Decimal> opt_dec(const json& obj, const char* key) {
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_string() || it->get_ref<const std::string&>().empty()) {
        return std::nullopt;
    }
    auto d = Decimal::parse(it->get_ref<const std::string&>());
    if (!d) return std::nullopt;
    return *d;
}

Result<Decimal> req_dec(const json& v, const char* what) {
    if (v.is_string()) return Decimal::parse(v.get_ref<const std::string&>());
    return fail(ErrorCode::parse_error, std::string("expected decimal string: ") + what);
}

std::vector<Level> parse_levels(const json& arr) {
    std::vector<Level> out;
    if (!arr.is_array()) return out;
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

BybitConfig parse_config(const YAML::Node& node) {
    BybitConfig c;
    if (node && node.IsMap()) {
        c.rest_base_url = config::opt_str(node, "rest_base_url", c.rest_base_url);
        c.ws_url = config::opt_str(node, "ws_url", c.ws_url);
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
        c.book_depth = static_cast<int32_t>(config::opt_int(node, "book_depth", c.book_depth));
    }
    return c;
}

BybitLinearMarketDataAdapter::BybitLinearMarketDataAdapter(BybitConfig cfg,
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

Result<json> BybitLinearMarketDataAdapter::get_result(const std::string& target) {
    auto res = get_with_retry(client_, bucket_, target, cfg_.max_retries, cfg_.backoff_min_ms,
                              cfg_.backoff_max_ms, should_stop_);
    if (!res) return std::unexpected(res.error());
    if (res->status != 200) return std::unexpected(map_exchange_error(res->status, res->body));
    json doc = json::parse(res->body, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        return fail(ErrorCode::parse_error, "invalid JSON from " + target);
    }
    if (opt_ll(doc, "retCode") != 0) {
        return std::unexpected(map_exchange_error(res->status, res->body));
    }
    if (!doc.contains("result")) {
        return fail(ErrorCode::parse_error, "missing result in response from " + target);
    }
    return doc["result"];
}

Result<std::vector<InstrumentSpec>> BybitLinearMarketDataAdapter::parse_instruments(
    const json& list) {
    if (!list.is_array()) return fail(ErrorCode::parse_error, "instruments list is not an array");
    std::vector<InstrumentSpec> out;
    for (const auto& s : list) {
        if (!s.is_object()) continue;
        if (s.value("contractType", "") != "LinearPerpetual") continue;
        if (s.value("quoteCoin", "") != "USDT") continue;
        if (s.value("settleCoin", "") != "USDT") continue;

        InstrumentSpec spec;
        spec.exchange_symbol = s.value("symbol", "");
        spec.base_asset = s.value("baseCoin", "");
        spec.quote_asset = "USDT";
        spec.settle_asset = "USDT";
        spec.contract_type = "perpetual";
        const std::string status = s.value("status", "");
        if (status == "Trading") {
            spec.status = InstrumentStatus::active;
        } else if (status == "PreLaunch" || status == "Settling" || status == "Delivering") {
            spec.status = InstrumentStatus::suspended;
        } else if (status == "Closed") {
            spec.status = InstrumentStatus::delisted;
        } else {
            spec.status = InstrumentStatus::suspended;
        }
        spec.list_time_ms = opt_ll(s, "launchTime");
        if (s.contains("priceFilter") && s["priceFilter"].is_object()) {
            if (auto d = opt_dec(s["priceFilter"], "tickSize")) {
                spec.tick_size = *d;
                spec.price_precision = d->scale();
            }
        }
        if (s.contains("lotSizeFilter") && s["lotSizeFilter"].is_object()) {
            const auto& lf = s["lotSizeFilter"];
            if (auto d = opt_dec(lf, "qtyStep")) {
                spec.quantity_step = *d;
                spec.quantity_precision = d->scale();
            }
            if (auto d = opt_dec(lf, "minOrderQty")) spec.min_order_quantity = *d;
            if (auto d = opt_dec(lf, "minNotionalValue")) spec.min_notional = *d;
        }
        if (s.contains("leverageFilter") && s["leverageFilter"].is_object()) {
            const auto& lv = s["leverageFilter"];
            // leverage strings like "100.00": parse integer part conservatively
            const std::string maxl = lv.value("maxLeverage", "");
            if (!maxl.empty()) {
                try { spec.max_leverage = static_cast<int32_t>(std::stod(maxl)); }
                catch (const std::exception&) { spec.max_leverage = 0; }
            }
            const std::string minl = lv.value("minLeverage", "");
            if (!minl.empty()) {
                try { spec.min_leverage = static_cast<int32_t>(std::stod(minl)); }
                catch (const std::exception&) { spec.min_leverage = 0; }
            }
        }
        const int64_t funding_min = opt_ll(s, "fundingInterval"); // minutes
        spec.funding_interval_hours = static_cast<int32_t>(funding_min / 60);
        if (spec.exchange_symbol.empty() || spec.base_asset.empty()) continue;
        out.push_back(std::move(spec));
    }
    return out;
}

Result<std::vector<InstrumentSpec>> BybitLinearMarketDataAdapter::list_instruments() {
    std::vector<InstrumentSpec> all;
    std::string cursor;
    for (int page = 0; page < 10; ++page) {
        std::string target = "/v5/market/instruments-info?category=linear&limit=1000";
        if (!cursor.empty()) target += "&cursor=" + cursor;
        auto result = get_result(target);
        if (!result) return std::unexpected(result.error());
        auto specs = parse_instruments((*result)["list"]);
        if (!specs) return std::unexpected(specs.error());
        for (auto& s : *specs) all.push_back(std::move(s));
        cursor = result->value("nextPageCursor", "");
        if (cursor.empty()) break;
    }
    return all;
}

Result<InstrumentSpec> BybitLinearMarketDataAdapter::get_instrument_spec(
    const std::string& exchange_symbol) {
    auto result =
        get_result("/v5/market/instruments-info?category=linear&symbol=" + exchange_symbol);
    if (!result) return std::unexpected(result.error());
    auto specs = parse_instruments((*result)["list"]);
    if (!specs) return std::unexpected(specs.error());
    for (auto& s : *specs) {
        if (s.exchange_symbol == exchange_symbol) return std::move(s);
    }
    return fail(ErrorCode::not_found, "contract not found: " + exchange_symbol);
}

Result<std::vector<Candle>> BybitLinearMarketDataAdapter::parse_klines(const json& list,
                                                                       Timeframe tf,
                                                                       CandleSource source,
                                                                       int64_t now_ms) {
    if (!list.is_array()) return fail(ErrorCode::parse_error, "kline list is not an array");
    std::vector<Candle> out;
    out.reserve(list.size());
    const int64_t span = tf_ms(tf);
    for (const auto& row : list) {
        // [start, open, high, low, close, volume(base), turnover(quote)] — strings.
        if (!row.is_array() || row.size() < 7) {
            return fail(ErrorCode::parse_error, "kline row has unexpected shape");
        }
        Candle c;
        c.timeframe = tf;
        if (!row[0].is_string()) return fail(ErrorCode::parse_error, "kline start not a string");
        try {
            c.open_time_ms = std::stoll(row[0].get_ref<const std::string&>());
        } catch (const std::exception&) {
            return fail(ErrorCode::parse_error, "invalid kline start time");
        }
        c.close_time_ms = c.open_time_ms + span;
        auto o = req_dec(row[1], "open"), h = req_dec(row[2], "high"), l = req_dec(row[3], "low"),
             cl = req_dec(row[4], "close"), bv = req_dec(row[5], "volume"),
             qv = req_dec(row[6], "turnover");
        for (const auto* r : {&o, &h, &l, &cl, &bv, &qv}) {
            if (!*r) return std::unexpected(r->error());
        }
        c.open = *o;
        c.high = *h;
        c.low = *l;
        c.close = *cl;
        c.base_volume = *bv;
        c.quote_volume = *qv;
        c.is_closed = c.close_time_ms <= now_ms;
        c.source = source;
        c.quality = Quality::ok;
        if (!c.valid()) {
            return fail(ErrorCode::validation_error,
                        "invalid kline at open_time " + std::to_string(c.open_time_ms));
        }
        out.push_back(std::move(c));
    }
    // v5 returns newest-first; the platform contract is ascending.
    std::sort(out.begin(), out.end(),
              [](const Candle& a, const Candle& b) { return a.open_time_ms < b.open_time_ms; });
    return out;
}

Result<CandlePage> BybitLinearMarketDataAdapter::fetch_candles(const std::string& exchange_symbol,
                                                               Timeframe tf, int64_t start_ms,
                                                               int64_t end_ms,
                                                               CandleQueryKind kind) {
    const int64_t span = tf_ms(tf);
    int64_t window_start = start_ms;
    if (end_ms > 0) {
        const int64_t page_span = static_cast<int64_t>(kKlinePageLimit) * span;
        if (end_ms - window_start > page_span) window_start = end_ms - page_span;
    }
    std::string target = "/v5/market/kline?category=linear&symbol=" + exchange_symbol +
                         "&interval=1&limit=" + std::to_string(kKlinePageLimit);
    if (window_start > 0) target += "&start=" + std::to_string(window_start);
    if (end_ms > 0) target += "&end=" + std::to_string(end_ms - 1);

    auto result = get_result(target);
    if (!result) return std::unexpected(result.error());
    const auto source = kind == CandleQueryKind::historical ? CandleSource::rest_bootstrap
                                                            : CandleSource::rest_reconcile;
    auto candles = parse_klines((*result)["list"], tf, source, timeutil::now_ms());
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


// Historical mark and index prices. Bybit serves each as its own kline
// endpoint, newest-first, paginated backwards on `end`.
Result<std::vector<MarkIndexSample>> BybitLinearMarketDataAdapter::fetch_mark_index_history(
    const std::string& exchange_symbol, int64_t from_ms, int64_t to_ms) {
    const auto series = [&](const char* path) -> Result<std::map<int64_t, Decimal>> {
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
            std::string target = std::string(path) + "?category=linear&symbol=" + exchange_symbol +
                                 "&interval=1&limit=" + std::to_string(kKlinePageLimit);
            if (window_start > 0) target += "&start=" + std::to_string(window_start);
            if (cursor_end > 0) target += "&end=" + std::to_string(cursor_end - 1);

            auto result = get_result(target);
            if (!result) {
                if (!out.empty()) break;
                return std::unexpected(result.error());
            }
            const auto& list = (*result)["list"];
            if (!list.is_array() || list.empty()) break;

            int64_t oldest = 0;
            for (const auto& row : list) {
                if (!row.is_array() || row.size() < 5) continue;
                auto ts_txt = row[0].is_string() ? row[0].get<std::string>() : std::string();
                if (ts_txt.empty()) continue;
                const int64_t ts = std::strtoll(ts_txt.c_str(), nullptr, 10);
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

    auto mark = series("/v5/market/mark-price-kline");
    if (!mark) return std::unexpected(mark.error());
    auto index = series("/v5/market/index-price-kline");
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

Result<std::vector<FundingSettlement>> BybitLinearMarketDataAdapter::fetch_funding_history(
    const std::string& exchange_symbol, int64_t from_ms, int64_t to_ms) {
    // Walk backwards on `endTime`. One page is 200 settlements -- about 66 days
    // at an 8-hour interval -- so a single request cannot satisfy a 180-day
    // window, and this venue sat at 70 days.
    std::vector<FundingSettlement> out;
    int64_t cursor = to_ms;
    bool reached_window_start = false;

    for (int page = 0; page < kFundingMaxPages && !reached_window_start; ++page) {
        if (should_stop_ && should_stop_()) break;
        std::string target = "/v5/market/funding/history?category=linear&symbol=" +
                             exchange_symbol + "&limit=" + std::to_string(kFundingPageLimit);
        if (from_ms > 0) target += "&startTime=" + std::to_string(from_ms);
        if (cursor > 0) target += "&endTime=" + std::to_string(cursor);

        auto result = get_result(target);
        if (!result) {
            if (!out.empty()) break;
            return std::unexpected(result.error());
        }
        const auto& list = (*result)["list"];
        if (!list.is_array()) return fail(ErrorCode::parse_error, "funding list not array");
        if (list.empty()) break; // venue boundary

        int64_t oldest = 0;
        int accepted = 0;
        for (const auto& r : list) {
            FundingSettlement f;
            f.funding_time_ms = opt_ll(r, "fundingRateTimestamp");
            auto rate = opt_dec(r, "fundingRate");
            if (!rate || f.funding_time_ms <= 0) continue;
            if (oldest == 0 || f.funding_time_ms < oldest) oldest = f.funding_time_ms;
            if (from_ms > 0 && f.funding_time_ms < from_ms) {
                reached_window_start = true;
                continue;
            }
            f.rate = *rate;
            out.push_back(std::move(f));
            ++accepted;
        }
        auto next = advance_funding_cursor(oldest, cursor, /*end_is_inclusive=*/true);
        if (!next) break;
        cursor = *next;
        if (accepted == 0 && !reached_window_start) break;
    }
    std::sort(out.begin(), out.end(), [](const FundingSettlement& a, const FundingSettlement& b) {
        return a.funding_time_ms < b.funding_time_ms;
    });
    return out;
}

Result<OpenInterestSample> BybitLinearMarketDataAdapter::fetch_open_interest(
    const std::string& exchange_symbol) {
    auto result = get_result("/v5/market/tickers?category=linear&symbol=" + exchange_symbol);
    if (!result) return std::unexpected(result.error());
    const auto& list = (*result)["list"];
    if (!list.is_array() || list.empty() || !list[0].is_object()) {
        return fail(ErrorCode::parse_error, "tickers list empty");
    }
    OpenInterestSample s;
    s.ts_ms = timeutil::now_ms();
    s.base = opt_dec(list[0], "openInterest");
    s.quote = opt_dec(list[0], "openInterestValue");
    if (!s.base && !s.quote) return fail(ErrorCode::parse_error, "openInterest missing");
    return s;
}

Error BybitLinearMarketDataAdapter::map_exchange_error(int http_status,
                                                       const std::string& body) const {
    int64_t code = 0;
    std::string msg;
    json doc = json::parse(body, nullptr, false);
    if (!doc.is_discarded() && doc.is_object()) {
        code = opt_ll(doc, "retCode");
        if (doc.contains("retMsg") && doc["retMsg"].is_string()) msg = doc["retMsg"];
    }
    const std::string detail = "bybit error http=" + std::to_string(http_status) +
                               " retCode=" + std::to_string(code) + " msg=" + msg.substr(0, 200);
    if (http_status == 403 || http_status == 429 || code == 10006) {
        return Error{ErrorCode::rate_limited, detail, true};
    }
    if (http_status >= 500) return Error{ErrorCode::upstream_error, detail, true};
    if (code == 10001 && msg.find("symbol") != std::string::npos) {
        return Error{ErrorCode::not_found, detail, false};
    }
    if (http_status == 400 || http_status == 404 || code == 10001) {
        return Error{ErrorCode::invalid_argument, detail, false};
    }
    return Error{ErrorCode::upstream_error, detail, false};
}

// ---------------------------------------------------------------------------
// WebSocket protocol
// ---------------------------------------------------------------------------

std::vector<std::string> BybitWsProtocol::subscribe_frames(
    const std::vector<std::string>& symbols) const {
    std::vector<std::string> frames;
    json args = json::array();
    const auto flush = [&] {
        if (args.empty()) return;
        frames.push_back(json{{"op", "subscribe"}, {"args", args}}.dump());
        args = json::array();
    };
    for (const auto& sym : symbols) {
        args.push_back("publicTrade." + sym);
        args.push_back("kline.1." + sym);
        args.push_back("orderbook." + std::to_string(cfg_.book_depth) + "." + sym);
        args.push_back("tickers." + sym);
        if (args.size() >= 40) flush();
    }
    flush();
    return frames;
}

std::vector<std::string> BybitWsProtocol::resubscribe_book_frames(const std::string& symbol) const {
    const std::string topic = "orderbook." + std::to_string(cfg_.book_depth) + "." + symbol;
    return {json{{"op", "unsubscribe"}, {"args", {topic}}}.dump(),
            json{{"op", "subscribe"}, {"args", {topic}}}.dump()};
}

bool BybitWsProtocol::parse(std::string_view frame, VenueEvents& out) {
    json doc = json::parse(frame, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) return false;

    if (doc.contains("op")) {
        const std::string op = doc.value("op", "");
        if (op == "ping" || op == "pong") {
            out.is_pong = true;
            return true;
        }
        if (op == "subscribe" && !doc.value("success", true)) {
            out.protocol_error = doc.value("ret_msg", "subscribe failed");
        }
        return true;
    }
    if (doc.contains("ret_msg") && doc.value("ret_msg", "") == "pong") {
        out.is_pong = true;
        return true;
    }
    const std::string topic = doc.value("topic", "");
    if (topic.empty()) return true;
    const std::string type = doc.value("type", "");
    const int64_t ts = opt_ll(doc, "ts");

    if (topic.rfind("publicTrade.", 0) == 0) {
        const auto& data = doc["data"];
        if (!data.is_array()) return true;
        for (const auto& t : data) {
            if (!t.is_object()) continue;
            VenueTradeEvent ev;
            ev.symbol = t.value("s", "");
            ev.trade.exchange_trade_id = t.value("i", "");
            auto p = opt_dec(t, "p");
            auto v = opt_dec(t, "v");
            if (!p || !v || ev.symbol.empty()) continue;
            ev.trade.price = *p;
            ev.trade.size = *v;
            const std::string side = t.value("S", "");
            ev.trade.side = side == "Buy" ? TradeSide::buy
                            : side == "Sell" ? TradeSide::sell
                                             : TradeSide::unspecified;
            ev.trade.exchange_ts_ms = opt_ll(t, "T");
            out.trades.push_back(std::move(ev));
        }
        return true;
    }
    if (topic.rfind("kline.", 0) == 0) {
        const std::string symbol = topic.substr(topic.rfind('.') + 1);
        const auto& data = doc["data"];
        if (!data.is_array()) return true;
        for (const auto& k : data) {
            if (!k.is_object()) continue;
            VenueCandleEvent c;
            c.symbol = symbol;
            c.candle.timeframe = Timeframe::m1;
            c.candle.open_time_ms = opt_ll(k, "start");
            c.candle.close_time_ms = c.candle.open_time_ms + timeutil::kMsPerMinute;
            auto o = opt_dec(k, "open"), h = opt_dec(k, "high"), l = opt_dec(k, "low"),
                 cl = opt_dec(k, "close"), v = opt_dec(k, "volume"), q = opt_dec(k, "turnover");
            if (!o || !h || !l || !cl || !v || !q) continue;
            c.candle.open = *o;
            c.candle.high = *h;
            c.candle.low = *l;
            c.candle.close = *cl;
            c.candle.base_volume = *v;
            c.candle.quote_volume = *q;
            c.candle.is_closed = k.value("confirm", false);
            c.candle.source = CandleSource::ws;
            c.candle.quality = Quality::ok;
            out.candles.push_back(std::move(c));
        }
        return true;
    }
    if (topic.rfind("orderbook.", 0) == 0) {
        const auto& data = doc["data"];
        if (!data.is_object()) return true;
        VenueBookEvent b;
        b.symbol = data.value("s", "");
        if (b.symbol.empty()) return true;
        const int64_t u = opt_ll(data, "u");
        // Official rule: type=snapshot (and u==1 service restart) resets the book.
        b.update.is_snapshot = (type == "snapshot") || u == 1;
        b.update.sequence = u;
        b.update.prev_sequence = -1;
        b.update.exchange_ms = opt_ll(doc, "cts") > 0 ? opt_ll(doc, "cts") : ts;
        b.update.bids = parse_levels(data.value("b", json::array()));
        b.update.asks = parse_levels(data.value("a", json::array()));
        out.books.push_back(std::move(b));
        return true;
    }
    if (topic.rfind("tickers.", 0) == 0) {
        const auto& data = doc["data"];
        if (!data.is_object()) return true;
        VenueTickerEvent t;
        t.symbol = data.value("symbol", "");
        if (t.symbol.empty()) return true;
        t.exchange_ms = ts;
        // Delta semantics: absent fields are unchanged — leave them unset and
        // let the runtime's last-value accumulator merge.
        t.last_price = opt_dec(data, "lastPrice");
        t.high_24h = opt_dec(data, "highPrice24h");
        t.low_24h = opt_dec(data, "lowPrice24h");
        t.base_volume_24h = opt_dec(data, "volume24h");
        t.quote_volume_24h = opt_dec(data, "turnover24h");
        t.mark_price = opt_dec(data, "markPrice");
        t.index_price = opt_dec(data, "indexPrice");
        t.funding_rate = opt_dec(data, "fundingRate");
        t.next_funding_time_ms = opt_ll(data, "nextFundingTime");
        t.open_interest_base = opt_dec(data, "openInterest");
        t.open_interest_quote = opt_dec(data, "openInterestValue");
        t.bid_price = opt_dec(data, "bid1Price");
        t.bid_size = opt_dec(data, "bid1Size");
        t.ask_price = opt_dec(data, "ask1Price");
        t.ask_size = opt_dec(data, "ask1Size");
        out.tickers.push_back(std::move(t));
        return true;
    }
    return true;
}

} // namespace altspotter::exchanges::bybit
