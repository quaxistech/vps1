#include "exchanges/bitget/bitget_context.hpp"
#include "exchanges/adapter_util.hpp"

namespace altspotter::exchanges::bitget {

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

} // namespace

std::vector<std::string> BitgetTickerWsProtocol::subscribe_frames(
    const std::vector<std::string>& symbols) const {
    std::vector<std::string> frames;
    json args = json::array();
    const auto flush = [&] {
        if (args.empty()) return;
        frames.push_back(json{{"op", "subscribe"}, {"args", args}}.dump());
        args = json::array();
    };
    for (const auto& sym : symbols) {
        args.push_back({{"instType", product_type_}, {"channel", "ticker"}, {"instId", sym}});
        if (args.size() >= 10) flush();
    }
    flush();
    return frames;
}

bool BitgetTickerWsProtocol::parse(std::string_view frame, VenueEvents& out) {
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
    if (doc["arg"].value("channel", "") != "ticker") return true;
    const auto& data = doc["data"];
    if (!data.is_array()) return true;

    for (const auto& t : data) {
        if (!t.is_object()) continue;
        VenueTickerEvent ev;
        ev.symbol = t.value("symbol", doc["arg"].value("instId", ""));
        if (ev.symbol.empty()) continue;
        ev.exchange_ms = opt_ll(t, "ts");
        ev.last_price = opt_dec(t, "lastPr");
        ev.high_24h = opt_dec(t, "high24h");
        ev.low_24h = opt_dec(t, "low24h");
        ev.open_24h = opt_dec(t, "open24h");
        ev.base_volume_24h = opt_dec(t, "baseVolume");
        ev.quote_volume_24h = opt_dec(t, "usdtVolume");
        ev.mark_price = opt_dec(t, "markPrice");
        ev.index_price = opt_dec(t, "indexPrice");
        ev.funding_rate = opt_dec(t, "fundingRate");
        ev.next_funding_time_ms = opt_ll(t, "nextFundingTime");
        ev.open_interest_base = opt_dec(t, "holdingAmount");
        ev.bid_price = opt_dec(t, "bidPr");
        ev.bid_size = opt_dec(t, "bidSz");
        ev.ask_price = opt_dec(t, "askPr");
        ev.ask_size = opt_dec(t, "askSz");
        out.tickers.push_back(std::move(ev));
    }
    return true;
}

} // namespace altspotter::exchanges::bitget
