#include "ofi_windows.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace alttech::ofi;

namespace {
Event event(uint64_t n, int64_t receipt, double flow = 1) {
    return {"connection-a", 1, n, receipt, flow, 10, true};
}
const Window& second(const std::vector<Window>& rows, int64_t start) {
    for (const auto& row : rows) if (row.span_ms == 1000 && row.start_ms == start) return row;
    FAIL("missing second window");
    return rows.front();
}
}

TEST_CASE("OFI windows preserve receipt duration and genuine flow units", "[ofi]") {
    Windows windows;
    windows.add(event(1, 1001));
    windows.add(event(2, 2001, 3));
    windows.add(event(3, 2500, -1));
    CHECK(windows.close(2999).size() == 1); // warm first second only
    windows.add(event(4, 3001));
    const auto rows = windows.close(4000);
    const auto& w = second(rows, 2000);
    CHECK(w.valid);
    CHECK(w.updates == 2);
    CHECK(w.flow_base == 2);
    CHECK(w.depth_normalized == Catch::Approx(0.2));
    CHECK(w.mean_depth_base == 10);
    CHECK(windows.close(6000).empty()); // no continuation proof for last second
}

TEST_CASE("OFI loss at bucket tail invalidates the bucket before publication", "[ofi]") {
    Windows windows;
    windows.add(event(1, 1001));
    windows.add(event(2, 2001));
    windows.add(event(4, 3001)); // counter3 lost from second2
    const auto rows = windows.close(4000);
    CHECK_FALSE(second(rows, 2000).valid);
}

TEST_CASE("OFI duplicates, reset and missing depth never become known flow", "[ofi]") {
    Windows windows;
    windows.add(event(1, 1001));
    windows.add(event(2, 2001, 2));
    windows.add(event(2, 2001, 2));
    auto reset = event(1, 2500);
    reset.epoch = 2;
    windows.add(reset);
    auto next = event(2, 3001);
    next.epoch = 2;
    windows.add(next);
    const auto rows = windows.close(4000);
    const auto& w = second(rows, 2000);
    CHECK(w.updates == 2);
    CHECK_FALSE(w.valid);
    next.counter = 3; next.received_ms = 3500; next.available = false;
    windows.add(next);
    next.counter = 4; next.received_ms = 4001; next.available = true;
    windows.add(next);
    CHECK_FALSE(second(windows.close(5000), 3000).valid);
}
