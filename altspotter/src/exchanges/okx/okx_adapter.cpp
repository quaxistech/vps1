#include <cstdlib>
#include <map>
#include "exchanges/pagination.hpp"
#include "exchanges/okx/okx_adapter.hpp"

#include "core/time.hpp"
#include "logging/logger.hpp"
#include "exchanges/adapter_util.hpp"

#include <algorithm>

namespace altspotter::exchanges::okx {

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

// OKX book levels: ["px","sz","0"(deprecated),"numOrders"]; sizes in contracts.
// The original strings are preserved for the crc32 checksum.
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

OkxConfig parse_config(const YAML::Node& node) {
    OkxConfig c;
    if (node && node.IsMap()) {
        c.rest_base_url = config::opt_str(node, "rest_base_url", c.rest_base_url);
        c.ws_public_url = config::opt_str(node, "ws_public_url", c.ws_public_url);
        c.ws_business_url = config::opt_str(node, "ws_business_url", c.ws_business_url);
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
    }
    return c;
}

OkxUsdtSwapMarketDataAdapter::OkxUsdtSwapMarketDataAdapter(OkxConfig cfg,
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

Result<json> OkxUsdtSwapMarketDataAdapter::get_data(const std::string& target) {
    auto res = get_with_retry(client_, bucket_, target, cfg_.max_retries, cfg_.backoff_min_ms,
                              cfg_.backoff_max_ms, should_stop_);
    if (!res) return std::unexpected(res.error());
    if (res->status != 200) return std::unexpected(map_exchange_error(res->status, res->body));
    json doc = json::parse(res->body, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        return fail(ErrorCode::parse_error, "invalid JSON from " + target);
    }
    if (doc.value("code", "") != "0") {
        return std::unexpected(map_exchange_error(res->status, res->body));
    }
    if (!doc.contains("data")) {
        return fail(ErrorCode::parse_error, "missing data in response from " + target);
    }
    return doc["data"];
}

Result<std::vector<InstrumentSpec>> OkxUsdtSwapMarketDataAdapter::parse_instruments(
    const json& data) {
    if (!data.is_array()) return fail(ErrorCode::parse_error, "instruments data is not an array");
    std::vector<InstrumentSpec> out;
    for (const auto& s : data) {
        if (!s.is_object()) continue;
        if (s.value("instType", "") != "SWAP") continue;
        if (s.value("ctType", "") != "linear") continue;
        if (s.value("settleCcy", "") != "USDT") continue;

        InstrumentSpec spec;
        spec.exchange_symbol = s.value("instId", "");
        spec.base_asset = s.value("ctValCcy", ""); // contract value currency = base
        spec.quote_asset = "USDT";
        spec.settle_asset = "USDT";
        spec.contract_type = "perpetual";
        if (auto d = opt_dec(s, "ctVal")) spec.contract_value = *d;
        const std::string state = s.value("state", "");
        if (state == "live") {
            spec.status = InstrumentStatus::active;
        } else if (state == "suspend" || state == "preopen" || state == "test") {
            spec.status = InstrumentStatus::suspended;
        } else if (state == "expired") {
            spec.status = InstrumentStatus::delisted;
        } else {
            spec.status = InstrumentStatus::suspended;
        }
        spec.list_time_ms = opt_ll(s, "listTime");
        if (auto d = opt_dec(s, "tickSz")) {
            spec.tick_size = *d;
            spec.price_precision = d->scale();
        }
        // lotSz/minSz are in contracts; normalize to base units via ctVal.
        if (auto d = opt_dec(s, "lotSz")) {
            if (auto step = d->mul(spec.contract_value)) {
                spec.quantity_step = *step;
                spec.quantity_precision = step->scale();
            }
        }
        if (auto d = opt_dec(s, "minSz")) {
            if (auto mq = d->mul(spec.contract_value)) spec.min_order_quantity = *mq;
        }
        const std::string lever = s.value("lever", "");
        if (!lever.empty()) {
            try { spec.max_leverage = static_cast<int32_t>(std::stod(lever)); }
            catch (const std::exception&) { spec.max_leverage = 0; }
        }
        if (spec.exchange_symbol.empty() || spec.base_asset.empty()) continue;
        out.push_back(std::move(spec));
    }
    return out;
}

Result<std::vector<InstrumentSpec>> OkxUsdtSwapMarketDataAdapter::list_instruments() {
    auto data = get_data("/api/v5/public/instruments?instType=SWAP");
    if (!data) return std::unexpected(data.error());
    return parse_instruments(*data);
}

Result<InstrumentSpec> OkxUsdtSwapMarketDataAdapter::get_instrument_spec(
    const std::string& exchange_symbol) {
    auto data =
        get_data("/api/v5/public/instruments?instType=SWAP&instId=" + exchange_symbol);
    if (!data) return std::unexpected(data.error());
    auto specs = parse_instruments(*data);
    if (!specs) return std::unexpected(specs.error());
    for (auto& s : *specs) {
        if (s.exchange_symbol == exchange_symbol) return std::move(s);
    }
    return fail(ErrorCode::not_found, "instrument not found: " + exchange_symbol);
}

Result<std::vector<Candle>> OkxUsdtSwapMarketDataAdapter::parse_candles(const json& data,
                                                                        Timeframe tf,
                                                                        CandleSource source,
                                                                        int64_t now_ms) {
    if (!data.is_array()) return fail(ErrorCode::parse_error, "candles data is not an array");
    std::vector<Candle> out;
    out.reserve(data.size());
    const int64_t span = tf_ms(tf);
    for (const auto& row : data) {
        // [ts,o,h,l,c,vol(contracts),volCcy(base),volCcyQuote(quote),confirm]
        if (!row.is_array() || row.size() < 9) {
            return fail(ErrorCode::parse_error, "candle row has unexpected shape");
        }
        Candle c;
        c.timeframe = tf;
        if (!row[0].is_string()) return fail(ErrorCode::parse_error, "candle ts not a string");
        try {
            c.open_time_ms = std::stoll(row[0].get_ref<const std::string&>());
        } catch (const std::exception&) {
            return fail(ErrorCode::parse_error, "invalid candle ts");
        }
        c.close_time_ms = c.open_time_ms + span;
        auto o = req_dec(row[1], "open"), h = req_dec(row[2], "high"), l = req_dec(row[3], "low"),
             cl = req_dec(row[4], "close"), bv = req_dec(row[6], "volCcy"),
             qv = req_dec(row[7], "volCcyQuote");
        for (const auto* r : {&o, &h, &l, &cl, &bv, &qv}) {
            if (!*r) return std::unexpected(r->error());
        }
        c.open = *o;
        c.high = *h;
        c.low = *l;
        c.close = *cl;
        c.base_volume = *bv;
        c.quote_volume = *qv;
        // `confirm` distinguishes the still-forming newest candle from finished
        // ones, but OKX also returns confirm="0" on sporadic historical minutes
        // (zero-volume fillers). A window that closed more than kConfirmGraceMs
        // ago is final regardless of the flag — otherwise those minutes would be
        // silently dropped and reported forever as unrepairable history gaps.
        const bool confirmed = row[8].is_string() && row[8].get_ref<const std::string&>() == "1";
        c.is_closed = c.close_time_ms <= now_ms &&
                      (confirmed || c.close_time_ms <= now_ms - kConfirmGraceMs);
        c.source = source;
        c.quality = Quality::ok;
        if (!c.valid()) {
            return fail(ErrorCode::validation_error,
                        "invalid candle at open_time " + std::to_string(c.open_time_ms));
        }
        out.push_back(std::move(c));
    }
    std::sort(out.begin(), out.end(),
              [](const Candle& a, const Candle& b) { return a.open_time_ms < b.open_time_ms; });
    return out;
}

Result<CandlePage> OkxUsdtSwapMarketDataAdapter::fetch_candles(const std::string& exchange_symbol,
                                                               Timeframe tf, int64_t start_ms,
                                                               int64_t end_ms,
                                                               CandleQueryKind kind) {
    const bool historical = kind == CandleQueryKind::historical;
    const int limit = historical ? kHistoryPageLimit : kRecentPageLimit;
    // "after" returns records strictly earlier than the given ts — this matches
    // the platform's exclusive end_ms walk contract exactly.
    std::string target = historical ? "/api/v5/market/history-candles"
                                    : "/api/v5/market/candles";
    target += "?instId=" + exchange_symbol + "&bar=1m&limit=" + std::to_string(limit);
    if (end_ms > 0) target += "&after=" + std::to_string(end_ms);

    auto data = get_data(target);
    if (!data) return std::unexpected(data.error());
    const auto source = historical ? CandleSource::rest_bootstrap : CandleSource::rest_reconcile;
    auto candles = parse_candles(*data, tf, source, timeutil::now_ms());
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


// Historical mark and index prices. OKX serves mark against the SWAP instId and
// index against the spot pair (BTC-USDT-SWAP -> BTC-USDT), both paginated
// backwards on `after`.
Result<std::vector<MarkIndexSample>> OkxUsdtSwapMarketDataAdapter::fetch_mark_index_history(
    const std::string& exchange_symbol, int64_t from_ms, int64_t to_ms) {
    const auto series = [&](const char* path, const char* id_key,
                            const std::string& id) -> Result<std::map<int64_t, Decimal>> {
        std::map<int64_t, Decimal> out;
        int64_t cursor_end = to_ms;
        for (int page = 0; page < kMarkIndexMaxPages; ++page) {
            if (should_stop_ && should_stop_()) break;
            std::string target = std::string(path) + "?" + id_key + "=" + id +
                                 "&bar=1m&limit=" + std::to_string(kHistoryPageLimit);
            if (cursor_end > 0) target += "&after=" + std::to_string(cursor_end);

            auto data = get_data(target);
            if (!data) {
                if (!out.empty()) break;
                return std::unexpected(data.error());
            }
            if (!data->is_array() || data->empty()) break;

            int64_t oldest = 0;
            for (const auto& row : *data) {
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

    // BTC-USDT-SWAP -> BTC-USDT for the index series.
    std::string index_id = exchange_symbol;
    const auto suffix = index_id.rfind("-SWAP");
    if (suffix != std::string::npos) index_id.erase(suffix);

    auto mark = series("/api/v5/market/history-mark-price-candles", "instId", exchange_symbol);
    if (!mark) return std::unexpected(mark.error());
    auto index = series("/api/v5/market/history-index-candles", "instId", index_id);
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

Result<std::vector<FundingSettlement>> OkxUsdtSwapMarketDataAdapter::fetch_funding_history(
    const std::string& exchange_symbol, int64_t from_ms, int64_t to_ms) {
    // Walk backwards on `after` until the window start is reached or the venue
    // runs out. One page is 100 settlements -- about 33 days at OKX's 8-hour
    // interval -- so a single request could never satisfy the configured
    // 180-day window, and this venue sat at 36 days while Bitget had 90.
    std::vector<FundingSettlement> out;
    int64_t cursor = to_ms;
    bool reached_window_start = false;

    for (int page = 0; page < kFundingMaxPages && !reached_window_start; ++page) {
        if (should_stop_ && should_stop_()) break;
        std::string target = "/api/v5/public/funding-rate-history?instId=" + exchange_symbol +
                             "&limit=" + std::to_string(kFundingPageLimit);
        if (cursor > 0) target += "&after=" + std::to_string(cursor);

        auto data = get_data(target);
        if (!data) {
            // A partial history still yields a usable interval estimate.
            if (!out.empty()) break;
            return std::unexpected(data.error());
        }
        if (!data->is_array()) return fail(ErrorCode::parse_error, "funding history not array");
        if (data->empty()) break; // venue boundary

        int64_t oldest = 0;
        int accepted = 0;
        for (const auto& r : *data) {
            FundingSettlement f;
            f.funding_time_ms = opt_ll(r, "fundingTime");
            auto rate = opt_dec(r, "realizedRate");
            if (!rate) rate = opt_dec(r, "fundingRate");
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
        auto next = advance_funding_cursor(oldest, cursor, /*end_is_inclusive=*/false);
        if (!next) break;
        cursor = *next;
        if (accepted == 0 && !reached_window_start) break;
    }
    std::sort(out.begin(), out.end(), [](const FundingSettlement& a, const FundingSettlement& b) {
        return a.funding_time_ms < b.funding_time_ms;
    });
    return out;
}

Result<OpenInterestSample> OkxUsdtSwapMarketDataAdapter::fetch_open_interest(
    const std::string& exchange_symbol) {
    auto data =
        get_data("/api/v5/public/open-interest?instType=SWAP&instId=" + exchange_symbol);
    if (!data) return std::unexpected(data.error());
    if (!data->is_array() || data->empty() || !(*data)[0].is_object()) {
        return fail(ErrorCode::parse_error, "open-interest data empty");
    }
    const auto& r = (*data)[0];
    OpenInterestSample s;
    s.ts_ms = opt_ll(r, "ts");
    s.base = opt_dec(r, "oiCcy");   // base currency units
    s.quote = opt_dec(r, "oiUsd");  // USD value
    if (!s.base && !s.quote) return fail(ErrorCode::parse_error, "open interest missing");
    return s;
}

Error OkxUsdtSwapMarketDataAdapter::map_exchange_error(int http_status,
                                                       const std::string& body) const {
    std::string code;
    std::string msg;
    json doc = json::parse(body, nullptr, false);
    if (!doc.is_discarded() && doc.is_object()) {
        if (doc.contains("code") && doc["code"].is_string()) code = doc["code"];
        if (doc.contains("msg") && doc["msg"].is_string()) msg = doc["msg"];
    }
    const std::string detail = "okx error http=" + std::to_string(http_status) +
                               " code=" + (code.empty() ? "?" : code) +
                               " msg=" + msg.substr(0, 200);
    if (http_status == 429 || code == "50011" || code == "50013") {
        return Error{ErrorCode::rate_limited, detail, true};
    }
    if (http_status >= 500) return Error{ErrorCode::upstream_error, detail, true};
    if (code == "51001") return Error{ErrorCode::not_found, detail, false};
    if (http_status == 400 || http_status == 404) {
        return Error{ErrorCode::invalid_argument, detail, false};
    }
    return Error{ErrorCode::upstream_error, detail, false};
}

// ---------------------------------------------------------------------------
// WebSocket protocols
// ---------------------------------------------------------------------------

namespace {

// OKX publishes the index price on its own instrument, not on the swap:
// BTC-USDT-SWAP is priced against the BTC-USDT index. The mapping is a strict
// suffix rule for USDT swaps, so it inverts without a lookup table.
constexpr const char* kSwapSuffix = "-SWAP";

std::string index_inst_id(const std::string& swap) {
    const size_t n = std::strlen(kSwapSuffix);
    if (swap.size() > n && swap.compare(swap.size() - n, n, kSwapSuffix) == 0)
        return swap.substr(0, swap.size() - n);
    return swap;
}

std::string swap_from_index(const std::string& index) { return index + kSwapSuffix; }

json sub_args_for(const std::vector<std::string>& symbols,
                  const std::vector<const char*>& channels) {
    json args = json::array();
    for (const auto& sym : symbols) {
        for (const auto* ch : channels) {
            args.push_back({{"channel", ch}, {"instId", sym}});
        }
    }
    return args;
}

std::vector<std::string> batched_subscribe(const json& args, size_t batch) {
    std::vector<std::string> frames;
    json chunk = json::array();
    for (const auto& a : args) {
        chunk.push_back(a);
        if (chunk.size() >= batch) {
            frames.push_back(json{{"op", "subscribe"}, {"args", chunk}}.dump());
            chunk = json::array();
        }
    }
    if (!chunk.empty()) {
        frames.push_back(json{{"op", "subscribe"}, {"args", chunk}}.dump());
    }
    return frames;
}

} // namespace

std::vector<std::string> OkxPublicWsProtocol::subscribe_frames(
    const std::vector<std::string>& symbols) const {
    json args = sub_args_for(symbols, {"trades", "books", "tickers", "mark-price",
                                       "funding-rate", "open-interest"});
    // index-tickers keys off the INDEX instrument. Without it OKX was the only
    // venue with no index price at all (measured: 0 % coverage against 100 % on
    // Bitget/Binance/Bybit), which silently reduced the cross-venue index
    // consensus to three venues and made mark-vs-index basis uncomputable there.
    for (const auto& sym : symbols) {
        args.push_back({{"channel", "index-tickers"}, {"instId", index_inst_id(sym)}});
    }
    return batched_subscribe(args, 40);
}

std::vector<std::string> OkxPublicWsProtocol::resubscribe_book_frames(
    const std::string& symbol) const {
    const json arg = {{"channel", "books"}, {"instId", symbol}};
    return {json{{"op", "unsubscribe"}, {"args", {arg}}}.dump(),
            json{{"op", "subscribe"}, {"args", {arg}}}.dump()};
}

bool OkxPublicWsProtocol::parse(std::string_view frame, VenueEvents& out) {
    if (frame == "pong") {
        out.is_pong = true;
        return true;
    }
    json doc = json::parse(frame, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) return false;
    if (doc.contains("event")) {
        const std::string event = doc.value("event", "");
        if (event == "error") out.protocol_error = doc.value("msg", "subscribe error");
        return true;
    }
    if (!doc.contains("arg") || !doc.contains("data") || !doc["arg"].is_object()) return true;
    const std::string channel = doc["arg"].value("channel", "");
    const std::string symbol = doc["arg"].value("instId", "");
    const auto& data = doc["data"];
    if (channel.empty() || symbol.empty() || !data.is_array()) return true;

    if (channel == "trades") {
        Decimal ct_val{1, 0};
        if (const auto it = contract_values_.find(symbol); it != contract_values_.end()) {
            ct_val = it->second;
        }
        for (const auto& t : data) {
            if (!t.is_object()) continue;
            VenueTradeEvent ev;
            ev.symbol = symbol;
            ev.trade.exchange_trade_id = t.value("tradeId", "");
            auto px = opt_dec(t, "px");
            auto sz = opt_dec(t, "sz");
            if (!px || !sz) continue;
            ev.trade.price = *px;
            auto base = sz->mul(ct_val); // contracts -> base units
            ev.trade.size = base ? *base : *sz;
            const std::string side = t.value("side", "");
            ev.trade.side = side == "buy" ? TradeSide::buy
                            : side == "sell" ? TradeSide::sell
                                             : TradeSide::unspecified;
            ev.trade.exchange_ts_ms = opt_ll(t, "ts");
            out.trades.push_back(std::move(ev));
        }
        return true;
    }
    if (channel == "books") {
        const std::string action = doc.value("action", "");
        for (const auto& b : data) {
            if (!b.is_object()) continue;
            VenueBookEvent ev;
            ev.symbol = symbol;
            ev.update.is_snapshot = action == "snapshot";
            ev.update.sequence = opt_ll(b, "seqId");
            ev.update.prev_sequence = b.contains("prevSeqId") ? opt_ll(b, "prevSeqId") : -1;
            ev.update.exchange_ms = opt_ll(b, "ts");
            // The books-channel checksum field is deprecated on the live API
            // (its value is fixed to 0); integrity comes from seqId/prevSeqId
            // validation. Verify crc32 only for a real (non-zero) checksum.
            if (b.contains("checksum") && b["checksum"].is_number_integer() &&
                b["checksum"].get<int64_t>() != 0) {
                ev.update.checksum = b["checksum"].get<int64_t>();
                ev.update.has_checksum = true;
            }
            ev.update.bids = parse_levels(b.value("bids", json::array()));
            ev.update.asks = parse_levels(b.value("asks", json::array()));
            out.books.push_back(std::move(ev));
        }
        return true;
    }
    if (channel == "tickers") {
        for (const auto& t : data) {
            if (!t.is_object()) continue;
            VenueTickerEvent ev;
            ev.symbol = symbol;
            ev.exchange_ms = opt_ll(t, "ts");
            ev.last_price = opt_dec(t, "last");
            ev.open_24h = opt_dec(t, "open24h");
            ev.high_24h = opt_dec(t, "high24h");
            ev.low_24h = opt_dec(t, "low24h");
            ev.base_volume_24h = opt_dec(t, "volCcy24h"); // base units for linear swaps
            ev.bid_price = opt_dec(t, "bidPx");
            ev.bid_size = opt_dec(t, "bidSz");
            ev.ask_price = opt_dec(t, "askPx");
            ev.ask_size = opt_dec(t, "askSz");
            out.tickers.push_back(std::move(ev));
        }
        return true;
    }
    if (channel == "index-tickers") {
        for (const auto& t : data) {
            if (!t.is_object()) continue;
            VenueTickerEvent ev;
            ev.symbol = swap_from_index(symbol);   // back to the swap this prices
            ev.exchange_ms = opt_ll(t, "ts");
            ev.index_price = opt_dec(t, "idxPx");
            out.tickers.push_back(std::move(ev));
        }
        return true;
    }
    if (channel == "mark-price") {
        for (const auto& t : data) {
            if (!t.is_object()) continue;
            VenueTickerEvent ev;
            ev.symbol = symbol;
            ev.exchange_ms = opt_ll(t, "ts");
            ev.mark_price = opt_dec(t, "markPx");
            out.tickers.push_back(std::move(ev));
        }
        return true;
    }
    if (channel == "funding-rate") {
        for (const auto& t : data) {
            if (!t.is_object()) continue;
            VenueTickerEvent ev;
            ev.symbol = symbol;
            ev.exchange_ms = opt_ll(t, "ts");
            ev.funding_rate = opt_dec(t, "fundingRate");
            ev.next_funding_time_ms = opt_ll(t, "fundingTime");
            out.tickers.push_back(std::move(ev));
        }
        return true;
    }
    if (channel == "open-interest") {
        for (const auto& t : data) {
            if (!t.is_object()) continue;
            VenueTickerEvent ev;
            ev.symbol = symbol;
            ev.exchange_ms = opt_ll(t, "ts");
            ev.open_interest_base = opt_dec(t, "oiCcy");
            ev.open_interest_quote = opt_dec(t, "oiUsd");
            out.tickers.push_back(std::move(ev));
        }
        return true;
    }
    return true;
}

std::vector<std::string> OkxBusinessWsProtocol::subscribe_frames(
    const std::vector<std::string>& symbols) const {
    return batched_subscribe(sub_args_for(symbols, {"candle1m"}), 40);
}

bool OkxBusinessWsProtocol::parse(std::string_view frame, VenueEvents& out) {
    if (frame == "pong") {
        out.is_pong = true;
        return true;
    }
    json doc = json::parse(frame, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) return false;
    if (doc.contains("event")) {
        if (doc.value("event", "") == "error") {
            out.protocol_error = doc.value("msg", "subscribe error");
        }
        return true;
    }
    if (!doc.contains("arg") || !doc.contains("data") || !doc["arg"].is_object()) return true;
    if (doc["arg"].value("channel", "") != "candle1m") return true;
    const std::string symbol = doc["arg"].value("instId", "");
    const auto& data = doc["data"];
    if (symbol.empty() || !data.is_array()) return true;

    for (const auto& row : data) {
        auto candles = OkxUsdtSwapMarketDataAdapter::parse_candles(
            json::array({row}), Timeframe::m1, CandleSource::ws, timeutil::now_ms());
        if (!candles || candles->empty()) continue;
        VenueCandleEvent ev;
        ev.symbol = symbol;
        ev.candle = std::move(candles->front());
        // WS confirm flag alone decides closedness for live candles.
        if (row.is_array() && row.size() >= 9 && row[8].is_string()) {
            ev.candle.is_closed = row[8].get_ref<const std::string&>() == "1";
        }
        out.candles.push_back(std::move(ev));
    }
    return true;
}

} // namespace altspotter::exchanges::okx
