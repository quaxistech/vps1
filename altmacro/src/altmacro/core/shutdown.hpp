#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace altmacro {

// Cooperative shutdown token. install_signal_handlers() hooks SIGINT/SIGTERM.
// Services poll stop_requested() or block in wait_for().
class ShutdownToken {
public:
    static ShutdownToken& instance();

    void install_signal_handlers();

    void request_stop() noexcept;

    // Async-signal-safe variant: atomic store only, no cv notify. Waiters use
    // short bounded waits, so the store is observed within one slice.
    void request_stop_from_signal() noexcept;

    [[nodiscard]] bool stop_requested() const noexcept {
        return stop_.load(std::memory_order_acquire);
    }

    // Returns true if stop was requested before the timeout elapsed.
    bool wait_for(std::chrono::milliseconds timeout);

private:
    ShutdownToken() = default;
    std::atomic<bool> stop_{false};
    std::mutex mu_;
    std::condition_variable cv_;
};

} // namespace altmacro
