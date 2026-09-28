#pragma once

#include "config/config.hpp"
#include "exchanges/binance/binance_adapter.hpp"
#include "exchanges/bitget/bitget_adapter.hpp"
#include "exchanges/bybit/bybit_adapter.hpp"
#include "exchanges/okx/okx_adapter.hpp"
#include "exchanges/market_data_adapter.hpp"
#include "exchanges/venues.hpp"

#include <functional>
#include <memory>

namespace altspotter::exchanges {

// Resolve one venue's REST config node from the unified config tree. Bitget's
// REST config lives under the catalog section's `bitget:` sub-node (unchanged);
// the other three venues reuse their runtime section (rest+ws fields together).
inline YAML::Node venue_rest_node(const config::ServiceContext& ctx, int16_t exchange_id) {
    switch (exchange_id) {
        case kVenueBitget:
            return ctx.service["bitget"] ? ctx.service["bitget"]
                                         : ctx.root["instrument_catalog"]["bitget"];
        case kVenueBinanceUsdm: return ctx.root["binance_usdm_runtime"];
        case kVenueBybit: return ctx.root["bybit_runtime"];
        case kVenueOkx: return ctx.root["okx_runtime"];
        default: return YAML::Node();
    }
}

inline bool venue_enabled(const config::ServiceContext& ctx, std::string_view code) {
    const auto ex = ctx.root["exchanges"];
    if (!ex || !ex[std::string(code)]) return true;
    return config::opt_bool(ex[std::string(code)], "enabled", true);
}

// Build a REST market-data adapter for a venue from its config node.
inline std::unique_ptr<ExchangeMarketDataAdapter> make_adapter(int16_t exchange_id,
                                                               const YAML::Node& node,
                                                               std::function<bool()> stop) {
    switch (exchange_id) {
        case kVenueBitget: {
            auto cfg = config::parse_bitget_rest(node);
            if (!cfg) return nullptr;
            return std::make_unique<bitget::BitgetMarketDataAdapter>(*cfg, std::move(stop));
        }
        case kVenueBinanceUsdm:
            return std::make_unique<binance::BinanceUsdmMarketDataAdapter>(
                binance::parse_config(node), std::move(stop));
        case kVenueBybit:
            return std::make_unique<bybit::BybitLinearMarketDataAdapter>(bybit::parse_config(node),
                                                                         std::move(stop));
        case kVenueOkx:
            return std::make_unique<okx::OkxUsdtSwapMarketDataAdapter>(okx::parse_config(node),
                                                                       std::move(stop));
        default:
            return nullptr;
    }
}

} // namespace altspotter::exchanges
