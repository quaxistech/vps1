#pragma once

#include <atomic>
#include <cstdint>
#include <string_view>

namespace altspotter {

// Process-wide persistence mode, owned by retention_manager and consulted by
// every raw-data writer before enqueueing rows. Under storage pressure the
// highest-volume ephemeral data is shed first while live NATS publishing,
// candles and the compact 1m context always continue.
enum class PersistenceMode : int32_t {
    full = 0,                  // FULL
    book_storage_disabled = 1, // DEGRADED_BOOK_STORAGE_DISABLED
    raw_trades_disabled = 2,   // DEGRADED_RAW_TRADES_DISABLED (books off too)
    core_only = 3,             // CRITICAL_CORE_ONLY (candles/context only)
};

[[nodiscard]] constexpr std::string_view persistence_mode_label(PersistenceMode m) noexcept {
    switch (m) {
        case PersistenceMode::full: return "FULL";
        case PersistenceMode::book_storage_disabled: return "DEGRADED_BOOK_STORAGE_DISABLED";
        case PersistenceMode::raw_trades_disabled: return "DEGRADED_RAW_TRADES_DISABLED";
        case PersistenceMode::core_only: return "CRITICAL_CORE_ONLY";
    }
    return "?";
}

class PersistenceGate {
public:
    static PersistenceGate& instance() {
        static PersistenceGate gate;
        return gate;
    }

    void set(PersistenceMode m) noexcept { mode_.store(static_cast<int32_t>(m)); }
    [[nodiscard]] PersistenceMode mode() const noexcept {
        return static_cast<PersistenceMode>(mode_.load(std::memory_order_relaxed));
    }

    [[nodiscard]] bool allow_book_snapshots() const noexcept {
        return mode() == PersistenceMode::full;
    }
    [[nodiscard]] bool allow_raw_trades() const noexcept {
        const auto m = mode();
        return m == PersistenceMode::full || m == PersistenceMode::book_storage_disabled;
    }

private:
    std::atomic<int32_t> mode_{0};
};

} // namespace altspotter
