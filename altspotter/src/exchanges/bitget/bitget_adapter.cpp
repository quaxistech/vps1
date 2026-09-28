#include "exchanges/pagination.hpp"
#include <map>
#include "exchanges/bitget/bitget_adapter.hpp"

#include "logging/logger.hpp"
#include "core/time.hpp"

#include <algorithm>

namespace altspotter::exchanges::bitget {

namespace {

std::string host_from_url(const std::string& url) {
    // https://api.bitget.com -> api.bitget.com
    const auto pos = url.find("://");
    std::string host = pos == std::string::npos ? url : url.substr(pos + 3);
    if (const auto slash = host.find('/'); slash != std::string::npos) host = host.substr(0, slash);
    return host;
}

Result<Decimal> parse_decimal_field(const nlohmann::json& obj, const char* key,
                                    bool required = false) {
    if (!obj.contains(key) || !obj[key].is_string() || obj[key].get_ref<const std::string&>().empty()) {
        if (required) {
            return fail(ErrorCode::parse_error, std::string("missing decimal field: ") + key);
        }
        return Decimal{};
    }
    return Decimal::parse(obj[key].get_ref<const std::string&>());
}

Result<int64_t> parse_ms_string(const nlohmann::json& v) {
    if (v.is_string()) {
        const std::string& s = v.get_ref<const std::string&>();
        if (s.empty()) return 0;
        try {
            return std::stoll(s);
        } catch (const std::exception&) {
            return fail(ErrorCode::parse_error, "invalid timestamp string: " + s);
        }
    }
    if (v.is_number_integer()) return v.get<int64_t>();
    return 0;
}

// Bitget granularity strings for supported timeframes.
const char* granularity_of(Timeframe tf) {
    switch (tf) {
        case Timeframe::m1: return "1m";
        case Timeframe::m5: return "5m";
        case Timeframe::m15: return "15m";
        case Timeframe::m30: return "30m";
        case Timeframe::h1: return "1H";
        case Timeframe::h4: return "4H";
        case Timeframe::d1: return "1Dutc";
    }
    return "1m";
}

} // namespace

BitgetMarketDataAdapter::BitgetMarketDataAdapter(config::BitgetRestConfig cfg,
                                                 std::function<bool()> should_stop)
    : cfg_(std::move(cfg)), should_stop_(std::move(should_stop)),
      client_(RestClientOptions{
          .host = host_from_url(cfg_.rest_base_url),
          .port = "443",
          .connect_timeout_ms = cfg_.connect_timeout_ms,
          .request_timeout_ms = cfg_.request_timeout_ms,
      }),
      bucket_(cfg_.rate_limit_per_second, std::max(2.0, cfg_.rate_limit_per_second)) {}

Result<nlohmann::json> BitgetMarketDataAdapter::get_json(const std::string& target) {
    auto res = get_with_retry(client_, bucket_, target, cfg_.max_retries, cfg_.backoff_min_ms,
                              cfg_.backoff_max_ms, should_stop_);
    if (!res) return std::unexpected(res.error());
    if (res->status != 200) {
        return std::unexpected(map_exchange_error(res->status, res->body));
    }
    nlohmann::json doc = nlohmann::json::parse(res->body, nullptr, false);
    if (doc.is_discarded()) {
        return fail(ErrorCode::parse_error, "invalid JSON from " + target);
    }
    if (!doc.contains("code") || doc["code"] != "00000") {
        return std::unexpected(map_exchange_error(res->status, res->body));
    }
    if (!doc.contains("data")) {
        return fail(ErrorCode::parse_error, "missing 'data' in response from " + target);
    }
    return doc;
}

Result<std::vector<InstrumentSpec>> BitgetMarketDataAdapter::parse_contracts(
    const nlohmann::json& payload, const std::string& product_type) {
    if (!payload.is_array()) {
        return fail(ErrorCode::parse_error, "contracts payload is not an array");
    }
    std::vector<InstrumentSpec> out;
    out.reserve(payload.size());

    for (const auto& c : payload) {
        if (!c.is_object() || !c.contains("symbol")) continue;

        InstrumentSpec s;
        s.exchange_symbol = c.value("symbol", "");
        s.base_asset = c.value("baseCoin", "");
        s.quote_asset = c.value("quoteCoin", "");
        const std::string symbol_type = c.value("symbolType", "");
        if (symbol_type != "perpetual") continue; // delivery futures are out of scope
        if (s.quote_asset != "USDT") continue;    // USDT-margined only (product_type guard)
        (void)product_type;
        s.contract_type = "perpetual";

        const std::string status = c.value("symbolStatus", "");
        if (status == "normal") {
            s.status = InstrumentStatus::active;
        } else if (status == "listed" || status == "maintain" || status == "limit_open" ||
                   status == "restrictedAPI") {
            s.status = InstrumentStatus::suspended;
        } else if (status == "off") {
            s.status = InstrumentStatus::delisted;
        } else {
            s.status = InstrumentStatus::suspended;
        }

        if (c.contains("launchTime")) {
            auto lt = parse_ms_string(c["launchTime"]);
            if (lt) s.list_time_ms = *lt > 0 ? *lt : 0;
        }

        // pricePlace = decimals; priceEndStep = step in last decimal place.
        const int64_t price_place = [&]() -> int64_t {
            if (!c.contains("pricePlace")) return 0;
            auto v = parse_ms_string(c["pricePlace"]);
            return v ? *v : 0;
        }();
        const int64_t price_end_step = [&]() -> int64_t {
            if (!c.contains("priceEndStep")) return 1;
            auto v = parse_ms_string(c["priceEndStep"]);
            return v && *v > 0 ? *v : 1;
        }();
        s.price_precision = static_cast<int32_t>(std::clamp<int64_t>(price_place, 0, 18));
        s.tick_size = Decimal{price_end_step, s.price_precision};

        const int64_t volume_place = [&]() -> int64_t {
            if (!c.contains("volumePlace")) return 0;
            auto v = parse_ms_string(c["volumePlace"]);
            return v ? *v : 0;
        }();
        s.quantity_precision = static_cast<int32_t>(std::clamp<int64_t>(volume_place, 0, 18));

        auto qty_step = parse_decimal_field(c, "sizeMultiplier");
        if (qty_step) s.quantity_step = *qty_step;
        auto min_qty = parse_decimal_field(c, "minTradeNum");
        if (min_qty) s.min_order_quantity = *min_qty;
        auto min_usdt = parse_decimal_field(c, "minTradeUSDT");
        if (min_usdt) s.min_notional = *min_usdt;
        auto maker = parse_decimal_field(c, "makerFeeRate");
        if (maker) s.maker_fee_rate = *maker;
        auto taker = parse_decimal_field(c, "takerFeeRate");
        if (taker) s.taker_fee_rate = *taker;

        const auto lever_of = [&](const char* key) -> int32_t {
            if (!c.contains(key)) return 0;
            auto v = parse_ms_string(c[key]);
            return v ? static_cast<int32_t>(std::clamp<int64_t>(*v, 0, 1000)) : 0;
        };
        s.min_leverage = lever_of("minLever");
        s.max_leverage = lever_of("maxLever");

        if (c.contains("fundInterval")) {
            auto v = parse_ms_string(c["fundInterval"]);
            if (v) s.funding_interval_hours = static_cast<int32_t>(std::clamp<int64_t>(*v, 0, 24));
        }

        out.push_back(std::move(s));
    }
    return out;
}

Result<std::vector<InstrumentSpec>> BitgetMarketDataAdapter::list_instruments() {
    const std::string target = "/api/v2/mix/market/contracts?productType=" + cfg_.product_type;
    auto doc = get_json(target);
    if (!doc) return std::unexpected(doc.error());
    return parse_contracts((*doc)["data"], cfg_.product_type);
}

Result<InstrumentSpec> BitgetMarketDataAdapter::get_instrument_spec(
    const std::string& exchange_symbol) {
    const std::string target = "/api/v2/mix/market/contracts?productType=" + cfg_.product_type +
                               "&symbol=" + exchange_symbol;
    auto doc = get_json(target);
    if (!doc) return std::unexpected(doc.error());
    auto specs = parse_contracts((*doc)["data"], cfg_.product_type);
    if (!specs) return std::unexpected(specs.error());
    for (auto& s : *specs) {
        if (s.exchange_symbol == exchange_symbol) return std::move(s);
    }
    return fail(ErrorCode::not_found, "contract not found: " + exchange_symbol);
}

Result<std::vector<Candle>> BitgetMarketDataAdapter::parse_candles(const nlohmann::json& payload,
                                                                   Timeframe tf,
                                                                   CandleSource source,
                                                                   int64_t now_ms) {
    if (!payload.is_array()) {
        return fail(ErrorCode::parse_error, "candles payload is not an array");
    }
    std::vector<Candle> out;
    out.reserve(payload.size());
    const int64_t span = tf_ms(tf);

    for (const auto& row : payload) {
        // [ts, open, high, low, close, baseVolume, quoteVolume]
        if (!row.is_array() || row.size() < 7) {
            return fail(ErrorCode::parse_error, "candle row has unexpected shape");
        }
        auto ts = parse_ms_string(row[0]);
        if (!ts) return std::unexpected(ts.error());

        Candle c;
        c.timeframe = tf;
        c.open_time_ms = *ts;
        c.close_time_ms = *ts + span;

        const auto dec = [&](size_t idx) -> Result<Decimal> {
            if (!row[idx].is_string()) {
                if (row[idx].is_number()) {
                    // Defensive: Bitget sends strings; refuse silent float coercion.
                    return fail(ErrorCode::parse_error, "numeric (non-string) decimal in candle");
                }
                return fail(ErrorCode::parse_error, "non-string decimal in candle");
            }
            return Decimal::parse(row[idx].get_ref<const std::string&>());
        };
        auto open = dec(1), high = dec(2), low = dec(3), close = dec(4), bvol = dec(5), qvol = dec(6);
        for (const auto* r : {&open, &high, &low, &close, &bvol, &qvol}) {
            if (!*r) return std::unexpected(r->error());
        }
        c.open = *open;
        c.high = *high;
        c.low = *low;
        c.close = *close;
        c.base_volume = *bvol;
        c.quote_volume = *qvol;
        c.is_closed = c.close_time_ms <= now_ms;
        c.source = source;
        c.quality = Quality::ok;

        if (!c.valid()) {
            return fail(ErrorCode::validation_error,
                        "invalid candle from exchange at open_time " +
                            std::to_string(c.open_time_ms));
        }
        out.push_back(std::move(c));
    }
    std::sort(out.begin(), out.end(),
              [](const Candle& a, const Candle& b) { return a.open_time_ms < b.open_time_ms; });
    return out;
}

int64_t BitgetMarketDataAdapter::page_window_start(int64_t start_ms, int64_t end_ms, int limit,
                                                   int64_t span_ms) noexcept {
    if (end_ms <= 0) return start_ms;
    int64_t window_start = start_ms;
    const int64_t page_span = static_cast<int64_t>(limit) * span_ms;
    if (end_ms - window_start > page_span) window_start = end_ms - page_span;
    // One page of 1d spans 200 days, still wider than the venue accepts.
    if (end_ms - window_start > kMaxQuerySpanMs) window_start = end_ms - kMaxQuerySpanMs;
    return window_start;
}

Result<CandlePage> BitgetMarketDataAdapter::fetch_candles(const std::string& exchange_symbol,
                                                          Timeframe tf, int64_t start_ms,
                                                          int64_t end_ms, CandleQueryKind kind) {
    const bool historical = kind == CandleQueryKind::historical;
    const int limit = historical ? kHistoryPageLimit : kRecentPageLimit;
    std::string target = historical ? "/api/v2/mix/market/history-candles"
                                    : "/api/v2/mix/market/candles";
    target += "?symbol=" + exchange_symbol + "&productType=" + cfg_.product_type +
              "&granularity=" + granularity_of(tf) + "&limit=" + std::to_string(limit);

    // Walk contract: return the latest page inside [start_ms, end_ms). The
    // caller walks `end_ms` backwards, so the start bound only has to cover one
    // page. Sending the caller's full `start_ms` on every request is what made
    // deep backfills fail: a 200-day bootstrap spans more than Bitget's 90-day
    // query range on its FIRST call and returns 40017 for every instrument.
    const int64_t window_start = page_window_start(start_ms, end_ms, limit, tf_ms(tf));
    if (window_start > 0) target += "&startTime=" + std::to_string(window_start);
    if (end_ms > 0) target += "&endTime=" + std::to_string(end_ms);

    auto doc = get_json(target);
    if (!doc) return std::unexpected(doc.error());

    const auto source = historical ? CandleSource::rest_bootstrap : CandleSource::rest_reconcile;
    auto candles = parse_candles((*doc)["data"], tf, source, timeutil::now_ms());
    if (!candles) return std::unexpected(candles.error());

    CandlePage page;
    page.exhausted = candles->empty();
    // Keep only closed candles inside [start_ms, end_ms).
    for (auto& c : *candles) {
        if (!c.is_closed) continue;
        if (start_ms > 0 && c.open_time_ms < start_ms) continue;
        if (end_ms > 0 && c.open_time_ms >= end_ms) continue;
        page.candles.push_back(std::move(c));
    }
    return page;
}


// Historical mark and index prices. Bitget serves them as separate candle
// endpoints with the same pagination contract as history-candles, so the same
// window clamp applies: a start/end pair wider than 90 days is rejected.
Result<std::vector<MarkIndexSample>> BitgetMarketDataAdapter::fetch_mark_index_history(
    const std::string& exchange_symbol, int64_t from_ms, int64_t to_ms) {
    const auto series = [&](const char* path) -> Result<std::map<int64_t, Decimal>> {
        std::map<int64_t, Decimal> out;
        int64_t cursor_end = to_ms;
        for (int page = 0; page < kMarkIndexMaxPages; ++page) {
            if (should_stop_ && should_stop_()) break;
            const int64_t ws = page_window_start(from_ms, cursor_end, kHistoryPageLimit,
                                                 tf_ms(Timeframe::m1));
            std::string target = std::string(path) + "?symbol=" + exchange_symbol +
                                 "&productType=" + cfg_.product_type + "&granularity=1m&limit=" +
                                 std::to_string(kHistoryPageLimit);
            if (ws > 0) target += "&startTime=" + std::to_string(ws);
            if (cursor_end > 0) target += "&endTime=" + std::to_string(cursor_end);

            auto doc = get_json(target);
            if (!doc) {
                if (!out.empty()) break;
                return std::unexpected(doc.error());
            }
            const auto& data = (*doc)["data"];
            if (!data.is_array() || data.empty()) break;

            int64_t oldest = 0;
            for (const auto& row : data) {
                if (!row.is_array() || row.size() < 5) continue;
                auto ts = parse_ms_string(row[0]);
                if (!ts || *ts <= 0) continue;
                auto close = Decimal::parse(row[4].is_string()
                                                ? row[4].get<std::string>()
                                                : std::to_string(row[4].get<double>()));
                if (!close) continue;
                if (from_ms > 0 && *ts < from_ms) continue;
                if (to_ms > 0 && *ts >= to_ms) continue;
                if (oldest == 0 || *ts < oldest) oldest = *ts;
                out.emplace(*ts, *close);
            }
            if (oldest <= 0) break;
            auto next = exchanges::advance_history_cursor(oldest, cursor_end);
            if (!next) break;
            cursor_end = *next;
            if (exchanges::history_walk_done(oldest, from_ms)) break;
        }
        return out;
    };

    auto mark = series("/api/v2/mix/market/history-mark-candles");
    if (!mark) return std::unexpected(mark.error());
    auto index = series("/api/v2/mix/market/history-index-candles");
    if (!index) return std::unexpected(index.error());

    // Union of both series: a minute present in only one is still worth storing,
    // with the other column left null rather than carried over from a neighbour.
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

// Funding settlement history. Bitget V2 exposes this only as a page-numbered
// feed ordered newest-first, with no time range parameters — unlike Binance's
// startTime/endTime — so the window is applied client-side by walking pages back
// until one predates `from_ms`.
//
// AltTech derives each venue's funding INTERVAL from the spacing of these
// settlements, and without it Bitget's funding contributes nothing to the
// cross-venue consensus: the primary venue was the only one with no settlement
// history at all, which left `bitget_funding_deviation` permanently unset.
Result<std::vector<FundingSettlement>> BitgetMarketDataAdapter::fetch_funding_history(
    const std::string& exchange_symbol, int64_t from_ms, int64_t to_ms) {
    std::vector<FundingSettlement> out;
    bool reached_window_start = false;

    for (int page = 1; page <= kFundingMaxPages && !reached_window_start; ++page) {
        if (should_stop_ && should_stop_()) break;
        const std::string target = "/api/v2/mix/market/history-fund-rate?symbol=" +
                                   exchange_symbol + "&productType=" + cfg_.product_type +
                                   "&pageSize=" + std::to_string(kFundingPageLimit) +
                                   "&pageNo=" + std::to_string(page);
        auto doc = get_json(target);
        if (!doc) {
            // A partial history is still usable for an interval estimate; only
            // fail when nothing at all could be read.
            if (!out.empty()) break;
            return std::unexpected(doc.error());
        }
        if (!doc->contains("data") || !(*doc)["data"].is_array()) {
            return fail(ErrorCode::parse_error, "history-fund-rate payload has no data array");
        }
        const auto& rows = (*doc)["data"];
        if (rows.empty()) break;   // walked past the listing date

        for (const auto& r : rows) {
            if (!r.contains("fundingTime")) continue;
            auto ts = parse_ms_string(r["fundingTime"]);
            if (!ts || *ts <= 0) continue;
            if (*ts < from_ms) { reached_window_start = true; continue; }
            if (to_ms > 0 && *ts > to_ms) continue;
            auto rate = parse_decimal_field(r, "fundingRate");
            if (!rate) continue;
            FundingSettlement f;
            f.funding_time_ms = *ts;
            f.rate = *rate;
            // This endpoint reports the rate only; the mark price at settlement
            // is not part of the payload and is left absent rather than guessed.
            out.push_back(std::move(f));
        }
        if (static_cast<int>(rows.size()) < kFundingPageLimit) break; // last page
    }

    std::sort(out.begin(), out.end(), [](const FundingSettlement& a, const FundingSettlement& b) {
        return a.funding_time_ms < b.funding_time_ms;
    });
    out.erase(std::unique(out.begin(), out.end(),
                          [](const FundingSettlement& a, const FundingSettlement& b) {
                              return a.funding_time_ms == b.funding_time_ms;
                          }),
              out.end());
    return out;
}

Error BitgetMarketDataAdapter::map_exchange_error(int http_status, const std::string& body) const {
    std::string biz_code;
    std::string biz_msg;
    nlohmann::json doc = nlohmann::json::parse(body, nullptr, false);
    if (!doc.is_discarded() && doc.is_object()) {
        if (doc.contains("code") && doc["code"].is_string()) biz_code = doc["code"];
        if (doc.contains("msg") && doc["msg"].is_string()) biz_msg = doc["msg"];
    }
    const std::string detail = "bitget error http=" + std::to_string(http_status) +
                               " code=" + (biz_code.empty() ? "?" : biz_code) + " msg=" +
                               (biz_msg.empty() ? "?" : biz_msg.substr(0, 200));

    if (http_status == 429 || biz_code == "429" || biz_code == "40429") {
        return Error{ErrorCode::rate_limited, detail, true};
    }
    if (http_status >= 500) return Error{ErrorCode::upstream_error, detail, true};
    if (biz_code == "40034" || biz_code == "40309") {
        // symbol does not exist / has been removed
        return Error{ErrorCode::not_found, detail, false};
    }
    if (http_status == 400 || http_status == 404) return Error{ErrorCode::invalid_argument, detail, false};
    return Error{ErrorCode::upstream_error, detail, false};
}

} // namespace altspotter::exchanges::bitget
