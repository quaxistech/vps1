#pragma once

#include "config/config.hpp"
#include "exchanges/venue_stream.hpp"

#include <nlohmann/json.hpp>

namespace altspotter::exchanges::bitget {

// Bitget v2 public "ticker" channel protocol for the derivatives-context
// runtime. Candles/trades/books for Bitget stay on the existing dedicated
// components (market_gateway / microstructure_collector) — this protocol only
// adds mark/index/funding/OI/turnover, which the ticker channel carries
// (fields mirror the REST /api/v2/mix/market/ticker payload verified live:
// lastPr, bidPr/askPr(+Sz), high24h/low24h, baseVolume, usdtVolume,
// indexPrice, markPrice, fundingRate, nextFundingTime, holdingAmount, ts).
class BitgetTickerWsProtocol final : public VenueWsProtocol {
public:
    BitgetTickerWsProtocol(std::string ws_url, std::string product_type)
        : ws_url_(std::move(ws_url)), product_type_(std::move(product_type)) {}

    [[nodiscard]] std::string ws_url() const override { return ws_url_; }
    [[nodiscard]] std::vector<std::string> subscribe_frames(
        const std::vector<std::string>& symbols) const override;
    [[nodiscard]] std::string ping_frame() const override { return "ping"; }
    [[nodiscard]] int ping_interval_sec() const override { return 25; }
    bool parse(std::string_view frame, VenueEvents& out) override;

private:
    std::string ws_url_;
    std::string product_type_;
};

} // namespace altspotter::exchanges::bitget
