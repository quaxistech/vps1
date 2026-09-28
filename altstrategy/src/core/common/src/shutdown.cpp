#include "altstrategy/common/shutdown.hpp"

#include <csignal>

namespace altstrategy {

namespace {
void handle_signal(int /*sig*/) { ShutdownToken::instance().request_stop(); }
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

void ShutdownToken::request_stop() noexcept {
    stop_.store(true, std::memory_order_release);
    cv_.notify_all();
}

bool ShutdownToken::wait_for(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mu_);
    return cv_.wait_for(lock, timeout, [this] { return stop_requested(); });
}

} // namespace altstrategy
