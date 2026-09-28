#include "alttech/application/module_supervisor.hpp"
#include "alttech/common/shutdown.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <future>
#include <thread>

TEST_CASE("module supervisor becomes ready only after every unique module") {
    alttech::application::ModuleSupervisor supervisor(2);

    supervisor.mark_ready("feature");
    supervisor.mark_ready("feature");
    CHECK(supervisor.ready_count() == 1);

    auto started = std::async(std::launch::async, [&] { return supervisor.wait_until_started(); });
    supervisor.mark_ready("regime");

    CHECK(started.get());
    CHECK(supervisor.ready_count() == 2);
    CHECK_FALSE(supervisor.failure().has_value());
}

TEST_CASE("module supervisor retains first failure and requests global stop") {
    alttech::application::ModuleSupervisor supervisor(4);
    const alttech::Error first{alttech::ErrorCode::database_error, "database unavailable", true};
    const alttech::Error second{alttech::ErrorCode::messaging_error, "NATS unavailable", true};

    supervisor.mark_failed("structure", first);
    supervisor.mark_failed("regime", second);

    REQUIRE(supervisor.failure().has_value());
    CHECK(supervisor.failure()->module == "structure");
    CHECK(supervisor.failure()->error.code == alttech::ErrorCode::database_error);
    CHECK_FALSE(supervisor.wait_until_started());
    CHECK(alttech::ShutdownToken::instance().stop_requested());
}

TEST_CASE("a withheld watchdog ping can name which module stalled") {
    alttech::application::ModuleSupervisor supervisor(2);

    // A module that never beacons is not judged, and must not be blamed.
    supervisor.heartbeat("cross_venue", 60'000);
    supervisor.heartbeat("feature", 60'000);
    CHECK(supervisor.all_alive());
    CHECK(supervisor.stalled_modules().empty());

    // An expired lease names exactly the module that let it expire. This is the
    // whole point: `all_alive() == false` alone cost a forensic reconstruction
    // to attribute 42 watchdog kills to cross_venue.
    supervisor.heartbeat("cross_venue", -1);      // ignored: non-positive lease
    CHECK(supervisor.stalled_modules().empty());

    supervisor.heartbeat("cross_venue", 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK_FALSE(supervisor.all_alive());

    const auto stalled = supervisor.stalled_modules();
    REQUIRE(stalled.size() == 1);
    CHECK(stalled.front().first == "cross_venue");
    CHECK(stalled.front().second >= 0);

    // Beaconing again clears it without touching the other module's lease.
    supervisor.heartbeat("cross_venue", 60'000);
    CHECK(supervisor.all_alive());
    CHECK(supervisor.stalled_modules().empty());
}
