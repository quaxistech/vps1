#include "exchanges/order_book.hpp"

#include <zlib.h>

namespace altspotter::exchanges {

void LocalBook::clear() {
    bids_.clear();
    asks_.clear();
    synced_ = false;
    last_seq_ = 0;
    last_exchange_ms_ = 0;
}

void LocalBook::reset_from_rest(std::vector<Level> bids, std::vector<Level> asks,
                                int64_t last_update_id, int64_t exchange_ms) {
    bids_.clear();
    asks_.clear();
    for (auto& l : bids) bids_.emplace(l.price, std::move(l));
    for (auto& l : asks) asks_.emplace(l.price, std::move(l));
    last_seq_ = last_update_id;
    last_exchange_ms_ = exchange_ms;
    synced_ = true;
    first_after_reset_ = true;
}

template <typename Map>
static void apply_levels(Map& side, const std::vector<Level>& levels) {
    for (const auto& l : levels) {
        if (l.size.is_zero()) {
            side.erase(l.price);
        } else {
            side.insert_or_assign(l.price, l);
        }
    }
}

void LocalBook::apply_side(std::map<Decimal, Level, std::greater<Decimal>>& side,
                           const std::vector<Level>& levels) {
    apply_levels(side, levels);
}

void LocalBook::apply_side(std::map<Decimal, Level, std::less<Decimal>>& side,
                           const std::vector<Level>& levels) {
    apply_levels(side, levels);
}

BookApplyResult LocalBook::apply(const BookUpdate& u) {
    if (u.is_snapshot) {
        bids_.clear();
        asks_.clear();
        apply_side(bids_, u.bids);
        apply_side(asks_, u.asks);
        last_seq_ = u.sequence;
        last_exchange_ms_ = u.exchange_ms;
        synced_ = true;
        first_after_reset_ = false;
        if (u.has_checksum && sequencing_ == Sequencing::okx &&
            okx_checksum() != static_cast<int32_t>(u.checksum)) {
            synced_ = false;
            return BookApplyResult::desync_checksum;
        }
        return crossed() ? BookApplyResult::crossed : BookApplyResult::reset;
    }

    switch (sequencing_) {
        case Sequencing::binance_futures: {
            if (!synced_) return BookApplyResult::waiting;
            // Official futures rule: drop events with u < lastUpdateId; the
            // first applied event must straddle lastUpdateId (U <= lastUpdateId
            // <= u); afterwards each event's pu must equal the previous u.
            if (first_after_reset_) {
                if (u.sequence < last_seq_) return BookApplyResult::ok; // pre-snapshot
                if (u.first_update_id > last_seq_) {
                    synced_ = false; // snapshot too old, events were missed
                    return BookApplyResult::desync_gap;
                }
                first_after_reset_ = false;
            } else if (u.prev_sequence != last_seq_) {
                synced_ = false;
                return BookApplyResult::desync_gap;
            }
            break;
        }
        case Sequencing::bybit: {
            if (!synced_) return BookApplyResult::waiting;
            // u is monotonically increasing between snapshots; a duplicate or
            // regression that is not a snapshot means a missed reset.
            if (u.sequence <= last_seq_) {
                synced_ = false;
                return BookApplyResult::desync_gap;
            }
            break;
        }
        case Sequencing::okx: {
            if (!synced_) return BookApplyResult::waiting;
            if (u.prev_sequence >= 0 && u.prev_sequence != last_seq_) {
                // seqId can repeat when there is no change; equal seq with
                // matching prev is a no-op keepalive.
                if (!(u.sequence == last_seq_ && u.prev_sequence == last_seq_)) {
                    synced_ = false;
                    return BookApplyResult::desync_gap;
                }
            }
            break;
        }
    }

    apply_side(bids_, u.bids);
    apply_side(asks_, u.asks);
    last_seq_ = u.sequence;
    if (u.exchange_ms > 0) last_exchange_ms_ = u.exchange_ms;

    if (u.has_checksum && sequencing_ == Sequencing::okx &&
        okx_checksum() != static_cast<int32_t>(u.checksum)) {
        synced_ = false;
        return BookApplyResult::desync_checksum;
    }
    return crossed() ? BookApplyResult::crossed : BookApplyResult::ok;
}

std::vector<Level> LocalBook::top_bids(size_t n) const {
    std::vector<Level> out;
    out.reserve(n);
    for (const auto& [p, l] : bids_) {
        if (out.size() >= n) break;
        out.push_back(l);
    }
    return out;
}

std::vector<Level> LocalBook::top_asks(size_t n) const {
    std::vector<Level> out;
    out.reserve(n);
    for (const auto& [p, l] : asks_) {
        if (out.size() >= n) break;
        out.push_back(l);
    }
    return out;
}

bool LocalBook::crossed() const {
    if (bids_.empty() || asks_.empty()) return false;
    return !(bids_.begin()->second.price < asks_.begin()->second.price);
}

int32_t LocalBook::okx_checksum() const {
    // Official OKX rule: concatenate the top 25 bids and asks alternately as
    // "bid_px:bid_sz:ask_px:ask_sz:..." (skipping exhausted sides), CRC32,
    // interpreted as signed int32.
    std::string payload;
    payload.reserve(1024);
    auto bid = bids_.begin();
    auto ask = asks_.begin();
    for (int i = 0; i < 25; ++i) {
        if (bid != bids_.end()) {
            if (!payload.empty()) payload.push_back(':');
            payload.append(bid->second.price_str).push_back(':');
            payload.append(bid->second.size_str);
            ++bid;
        }
        if (ask != asks_.end()) {
            if (!payload.empty()) payload.push_back(':');
            payload.append(ask->second.price_str).push_back(':');
            payload.append(ask->second.size_str);
            ++ask;
        }
    }
    const auto crc = crc32(0L, reinterpret_cast<const Bytef*>(payload.data()),
                           static_cast<uInt>(payload.size()));
    return static_cast<int32_t>(static_cast<uint32_t>(crc));
}

} // namespace altspotter::exchanges
