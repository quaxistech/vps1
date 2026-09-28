#include "alttech/common/shutdown.hpp"
#include "alttech/persistence/pg.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <thread>
#include <unistd.h>

using namespace std::chrono_literals;

TEST_CASE("SIGTERM safely wakes cooperative shutdown waiters") {
    auto& shutdown = alttech::ShutdownToken::instance();
    shutdown.install_signal_handlers();

    std::atomic<bool> awakened{false};
    std::thread waiter([&] { awakened.store(shutdown.wait_for(2s)); });

    // kill(2) sends a process-directed signal. Since SIGTERM was blocked before
    // the waiter was created, the dedicated sigwait thread must consume it and
    // perform the condition-variable notification from a normal thread.
    const int signal_result = ::kill(::getpid(), SIGTERM);
    waiter.join();

    REQUIRE(signal_result == 0);
    CHECK(shutdown.stop_requested());
    CHECK(awakened.load());
}

// A vanished schema must stop the process, not be retried forever. The
// threshold has to be high enough that one odd statement cannot stop a healthy
// service, and low enough that a genuinely broken one stops within seconds.
TEST_CASE("schema integrity is lost only after repeated schema failures") {
    using alttech::persistence::schema_integrity_lost;
    CHECK_FALSE(schema_integrity_lost(0));
    CHECK_FALSE(schema_integrity_lost(1));
    CHECK_FALSE(schema_integrity_lost(alttech::persistence::kSchemaFailureThreshold - 1));
    CHECK(schema_integrity_lost(alttech::persistence::kSchemaFailureThreshold));
    CHECK(schema_integrity_lost(alttech::persistence::kSchemaFailureThreshold + 100));
}
