#pragma once

#include "core/decimal.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace altspotter::exchanges {

// One price level of the reconstructed local book. The exact venue strings are
// kept alongside the parsed decimals because OKX checksums are computed over
// the original "price:size" text.
struct Level {
    Decimal price;
    Decimal size;
    std::string price_str;
    std::string size_str;
};

// A parsed order-book message (snapshot or delta) in venue-neutral form.
struct BookUpdate {
    bool is_snapshot = false;
    std::vector<Level> bids;   // size 0 in a delta deletes the level
    std::vector<Level> asks;
    int64_t first_update_id = 0;  // Binance U (0 when n/a)
    int64_t sequence = 0;         // Binance u / Bybit u / OKX seqId
    int64_t prev_sequence = -1;   // Binance pu / OKX prevSeqId (-1 when n/a)
    int64_t exchange_ms = 0;
    int64_t checksum = 0;         // OKX signed crc32; 0 = absent
    bool has_checksum = false;
};

enum class BookApplyResult {
    ok,
    reset,          // snapshot applied, book rebuilt
    waiting,        // delta buffered/skipped before initial snapshot
    desync_gap,     // sequence discontinuity — resync required
    desync_checksum,// checksum mismatch — resync required
    crossed,        // bid >= ask after apply — suspect state
    empty,          // a side vanished — no BBO, reseed required
    invalid,        // nonpositive price/negative quantity in a level
};

// Local order book with venue-specific sequencing validated by a pluggable
// rule. One instance per (venue, instrument); accessed from that venue's WS
// io thread only (sharded by design, no global locks).
class LocalBook {
public:
    // Sequencing contract, chosen per venue:
    //   binance_futures : need REST snapshot; first delta must satisfy
    //                     U <= lastUpdateId <= u; then pu == previous u.
    //   bybit           : WS sends the snapshot itself; delta valid when
    //                     u > previous u (u == 1 forces snapshot reset).
    //   okx             : WS sends the snapshot; delta valid when
    //                     prevSeqId == previous seqId; crc32 checksum verified.
    enum class Sequencing { binance_futures, bybit, okx };

    explicit LocalBook(Sequencing seq, uint64_t initial_epoch = 0)
        : sequencing_(seq), epoch_(initial_epoch) {}

    // Apply a snapshot or delta; returns the outcome (desync_* => caller must
    // trigger the venue resync flow and stop publishing VALID state).
    BookApplyResult apply(const BookUpdate& update);

    // Binance flow: seed from the REST depth snapshot (lastUpdateId).
    void reset_from_rest(std::vector<Level> bids, std::vector<Level> asks,
                         int64_t last_update_id, int64_t exchange_ms);

    void clear();

    [[nodiscard]] bool synced() const noexcept { return synced_; }
    [[nodiscard]] int64_t last_sequence() const noexcept { return last_seq_; }
    [[nodiscard]] int64_t last_exchange_ms() const noexcept { return last_exchange_ms_; }
    // Cont/Kukanov/Stoikov best-queue increment from the last successfully
    // applied delta. Snapshots, duplicates and failed continuity have no flow.
    [[nodiscard]] const std::optional<Decimal>& last_ofi() const noexcept { return last_ofi_; }
    [[nodiscard]] uint64_t epoch() const noexcept { return epoch_; }
    [[nodiscard]] uint64_t delta_counter() const noexcept { return delta_counter_; }
    [[nodiscard]] size_t bid_levels() const noexcept { return bids_.size(); }
    [[nodiscard]] size_t ask_levels() const noexcept { return asks_.size(); }

    // Best-first copies of the top N levels.
    [[nodiscard]] std::vector<Level> top_bids(size_t n) const;
    [[nodiscard]] std::vector<Level> top_asks(size_t n) const;

    [[nodiscard]] bool crossed() const;

    // OKX checksum: crc32 over "bid1_px:bid1_sz:ask1_px:ask1_sz:..." of the
    // top 25 levels per side (venue-original strings), signed int32.
    [[nodiscard]] int32_t okx_checksum() const;

private:
    void apply_side(std::map<Decimal, Level, std::greater<Decimal>>& side,
                    const std::vector<Level>& levels);
    void apply_side(std::map<Decimal, Level, std::less<Decimal>>& side,
                    const std::vector<Level>& levels);

    Sequencing sequencing_;
    bool synced_ = false;
    bool first_after_reset_ = false; // Binance: awaiting the straddling event
    int64_t last_seq_ = 0;
    int64_t last_exchange_ms_ = 0;
    std::optional<Decimal> last_ofi_;
    uint64_t epoch_ = 0;
    uint64_t delta_counter_ = 0;
    std::map<Decimal, Level, std::greater<Decimal>> bids_; // best (highest) first
    std::map<Decimal, Level, std::less<Decimal>> asks_;    // best (lowest) first
};

} // namespace altspotter::exchanges
