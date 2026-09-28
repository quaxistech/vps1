// AltTech common runtime implementation.
#include "alttech/common/shutdown.hpp"

#include <cerrno>
#include <csignal>
#include <pthread.h>
#include <system_error>

namespace alttech {

ShutdownToken& ShutdownToken::instance() {
    static ShutdownToken token;
    return token;
}

ShutdownToken::~ShutdownToken() {
    std::lock_guard lock(signal_mu_);
    if (!signal_thread_.joinable()) return;

    stop_signal_thread_.store(true, std::memory_order_release);
    // SIGTERM is blocked by the signal thread and therefore wakes sigwait()
    // without running user code in an asynchronous signal context.
    (void)pthread_kill(signal_thread_.native_handle(), SIGTERM);
    signal_thread_.join();
}

void ShutdownToken::install_signal_handlers() {
    std::lock_guard lock(signal_mu_);
    if (signal_handling_installed_) return;

    sigemptyset(&termination_signals_);
    sigaddset(&termination_signals_, SIGINT);
    sigaddset(&termination_signals_, SIGTERM);

    // This method is called during bootstrap, before NATS and module threads
    // are created. New threads inherit the blocked mask, so process-directed
    // termination signals can only be consumed by signal_loop().
    const int mask_result =
        pthread_sigmask(SIG_BLOCK, &termination_signals_, &previous_signal_mask_);
    if (mask_result != 0) {
        throw std::system_error(mask_result, std::generic_category(),
                                "failed to block termination signals");
    }

    struct sigaction ignore_pipe{};
    ignore_pipe.sa_handler = SIG_IGN;
    sigemptyset(&ignore_pipe.sa_mask);
    if (sigaction(SIGPIPE, &ignore_pipe, nullptr) != 0) {
        const int error = errno;
        (void)pthread_sigmask(SIG_SETMASK, &previous_signal_mask_, nullptr);
        throw std::system_error(error, std::generic_category(), "failed to ignore SIGPIPE");
    }

    try {
        signal_thread_ = std::thread(&ShutdownToken::signal_loop, this);
    } catch (...) {
        // Do not leave termination signals blocked when installation fails.
        (void)pthread_sigmask(SIG_SETMASK, &previous_signal_mask_, nullptr);
        throw;
    }
    signal_handling_installed_ = true;
}

void ShutdownToken::signal_loop() noexcept {
    for (;;) {
        int received = 0;
        const int result = sigwait(&termination_signals_, &received);
        if (stop_signal_thread_.load(std::memory_order_acquire)) return;
        if (result != 0) {
            // Losing termination handling is fatal to graceful lifecycle; make
            // the process converge on a coordinated stop instead of spinning.
            request_stop();
            return;
        }
        if (received == SIGINT || received == SIGTERM) request_stop();
    }
}

void ShutdownToken::request_stop() noexcept {
    stop_.store(true, std::memory_order_release);
    cv_.notify_all();
}

bool ShutdownToken::wait_for(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mu_);
    return cv_.wait_for(lock, timeout, [this] { return stop_requested(); });
}

} // namespace alttech
