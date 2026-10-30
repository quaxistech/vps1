// DT-03: alternative bars are causal transforms of closed bars; synthetic bars.

#include "event_bars.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace alttech::event_bars;
using Catch::Approx;

namespace {
SourceBar bar(int64_t t, double o, double h, double l, double c, double v = 1.0) {
    return SourceBar{t, o, h, l, c, v, v * c};
}
} // namespace

TEST_CASE("heikin-ashi is recursive on closed bars and restarts after a gap", "[event_bars]") {
    const auto ha = heikin_ashi({bar(1, 10, 12, 9, 11), bar(2, 11, 13, 10, 12)});
    REQUIRE(ha.size() == 2);
    CHECK(ha[0].close == Approx(10.5));
    CHECK(ha[0].open == Approx(10.5));
    CHECK(ha[1].open == Approx((ha[0].open + ha[0].close) / 2.0));
    CHECK(ha[1].close == Approx(11.5));
}

TEST_CASE("renko advances on closes only and needs two bricks to reverse", "[event_bars]") {
    const auto r = renko({bar(1, 100, 100, 100, 100), bar(2, 100, 103, 100, 103), bar(3, 103, 103, 101, 101.5),
                          bar(4, 101.5, 101.5, 99, 99)}, 1.0);
    REQUIRE(r.size() == 6);   // three up bricks; the fall to 99 reverses (two bricks) and runs on: three down
    CHECK(r[2].close == Approx(103));
    CHECK(r[3].direction == -1);
    CHECK(r[3].close == Approx(101));
}

TEST_CASE("range and flow bars close on accumulated range or real flow, never on a guess", "[event_bars]") {
    const auto rg = range_bars({bar(1, 10, 10.5, 9.8, 10.2), bar(2, 10.2, 11.0, 10.1, 10.9)}, 1.0);
    REQUIRE(rg.size() == 1);
    CHECK(rg[0].close_ms == 2);
    const auto vb = flow_bars({bar(1, 10, 10, 10, 10, 3), bar(2, 10, 11, 10, 11, 3)}, 5.0, false);
    REQUIRE(vb.size() == 1);
    CHECK(vb[0].volume == Approx(6));
    auto unknown = bar(1, 10, 10, 10, 10, 3);
    unknown.volume = -1;
    CHECK(flow_bars({unknown, bar(2, 10, 11, 10, 11, 3)}, 5.0, false).empty());
}

TEST_CASE("kagi and point-and-figure reverse only by their declared amounts", "[event_bars]") {
    const auto k = kagi({bar(1, 10, 10, 10, 10), bar(2, 10, 12, 10, 12), bar(3, 12, 12, 10.5, 10.5)}, 1.0);
    REQUIRE(k.size() == 1);
    CHECK(k[0].direction == 1);
    const auto pf = point_figure({bar(1, 10, 10, 10, 10), bar(2, 10, 14, 10, 14), bar(3, 14, 14, 10, 10)}, 1.0, 3);
    REQUIRE(pf.size() == 1);
    CHECK(pf[0].direction == 1);
}
