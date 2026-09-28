#pragma once

#include "config/config.hpp"
#include "exchanges/market_data_adapter.hpp"

#include <memory>
#include <string>
#include <vector>

namespace altspotter::exchanges::bitget {

// Bitget V2 public WebSocket stream manager.
//
// - Symbols are sharded across >=2 TLS connections (trade + candle1m channels
//   stay on the symbol's connection).
// - Application-level heartbeat: text "ping" every ws_ping_interval_sec, the
//   connection is recycled if no "pong"/traffic within ws_pong_timeout_sec.
// - Reconnect with exponential backoff + full jitter, then resubscribe.
// - Outgoing messages are rate limited; subscriptions are sent in batches.
// - Stale stream detection: no inbound frames within stale_stream_timeout_sec
//   forces a reconnect.
// - Controlled rotation: each connection is gracefully recycled every
//   ws_rotation_interval_sec (staggered per connection).
// - Candle close detection: a 1m candle is published as closed when a newer
//   open_time appears on the same symbol (values of the previous update are
//   final at that point).
//
// All callbacks fire on the io thread owned by the manager (single thread).
class BitgetWsManager final : public ExchangeMarketStream {
public:
    BitgetWsManager(config::BitgetWsConfig cfg, int connections);
    ~BitgetWsManager() override;

    void start(const std::vector<std::string>& symbols, StreamCallbacks callbacks) override;
    void update_symbols(const std::vector<std::string>& symbols) override;
    void stop() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace altspotter::exchanges::bitget
