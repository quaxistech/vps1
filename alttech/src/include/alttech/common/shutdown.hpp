#pragma once
// Internal AltTech runtime API.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <mutex>
#include <thread>

namespace alttech {

// Cooperative shutdown token. install_signal_handlers() blocks SIGINT/SIGTERM
// before worker threads are created and handles them synchronously on a
// dedicated sigwait thread. Services poll stop_requested() or block in
// wait_for().
class ShutdownToken {
public:
    static ShutdownToken& instance();

    ~ShutdownToken();

    void install_signal_handlers();

    void request_stop() noexcept;

    [[nodiscard]] bool stop_requested() const noexcept {
        return stop_.load(std::memory_order_acquire);
    }

    // Returns true if stop was requested before the timeout elapsed.
    bool wait_for(std::chrono::milliseconds timeout);

private:
    ShutdownToken() = default;

    void signal_loop() noexcept;

    std::atomic<bool> stop_{false};
    std::mutex mu_;
    std::condition_variable cv_;

    std::mutex signal_mu_;
    sigset_t termination_signals_{};
    sigset_t previous_signal_mask_{};
    std::thread signal_thread_;
    std::atomic<bool> stop_signal_thread_{false};
    bool signal_handling_installed_ = false;
};

} // namespace alttech
