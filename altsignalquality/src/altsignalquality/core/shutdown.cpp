#include "altsignalquality/core/shutdown.hpp"

#include <csignal>

namespace altsignalquality {

namespace {
// Signal handlers may only touch async-signal-safe state. The atomic store is
// safe; condition_variable::notify_all is NOT (it can deadlock or lose the
// wakeup if the signal lands inside the cv's own internals). Waiters therefore
// sleep in short bounded slices (see wait_for) so a handler-only store is
// observed within one slice, well inside the systemd stop timeout.
void handle_signal(int /*sig*/) { ShutdownToken::instance().request_stop_from_signal(); }
} // namespace

ShutdownToken& ShutdownToken::instance() {
    static ShutdownToken token;
    return token;
}

void ShutdownToken::install_signal_handlers() {
    struct sigaction sa{};
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);
}

void ShutdownToken::request_stop_from_signal() noexcept {
    stop_.store(true, std::memory_order_release); // async-signal-safe: store only
}

void ShutdownToken::request_stop() noexcept {
    stop_.store(true, std::memory_order_release);
    cv_.notify_all(); // safe: never called from a signal handler
}

bool ShutdownToken::wait_for(std::chrono::milliseconds timeout) {
    constexpr auto kSlice = std::chrono::milliseconds(200);
    std::unique_lock lock(mu_);
    auto remaining = timeout;
    while (!stop_requested()) {
        const auto step = remaining < kSlice ? remaining : kSlice;
        if (step <= std::chrono::milliseconds::zero()) return stop_requested();
        cv_.wait_for(lock, step, [this] { return stop_requested(); });
        remaining -= step;
    }
    return true;
}

} // namespace altsignalquality
