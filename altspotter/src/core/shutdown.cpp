#include "core/shutdown.hpp"

#include <csignal>
#include <pthread.h>
#include <thread>

namespace altspotter {

ShutdownToken& ShutdownToken::instance() {
    static ShutdownToken token;
    return token;
}

// request_stop() takes a mutex and notifies a condition variable — neither is
// async-signal-safe, so it must never run in signal-handler context. Instead
// SIGINT/SIGTERM are blocked in every thread (the mask is inherited by all
// threads created afterwards) and a dedicated thread consumes them via
// sigwait(), turning the delivery into an ordinary cross-thread call.
void ShutdownToken::install_signal_handlers() {
    signal(SIGPIPE, SIG_IGN);

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);

    std::thread([set] {
        for (;;) {
            int sig = 0;
            if (sigwait(&set, &sig) == 0) {
                ShutdownToken::instance().request_stop();
            }
        }
    }).detach();
}

void ShutdownToken::request_stop() noexcept {
    stop_.store(true, std::memory_order_release);
    cv_.notify_all();
}

bool ShutdownToken::wait_for(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mu_);
    return cv_.wait_for(lock, timeout, [this] { return stop_requested(); });
}

} // namespace altspotter
