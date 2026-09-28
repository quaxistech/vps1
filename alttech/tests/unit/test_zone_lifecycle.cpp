// Market-structure lifecycle: a zone's state must be a pure function of the bar
// window, and "broken" must mean what a trader means by it.
//
// AltSpotter revises 5-8 % of closed bars with a 95th percentile around seven
// hours after close, so any state accumulated bar by bar drifts away from the
// corrected data and never returns. Deriving it means recomputing after a
// revision produces the corrected answer — and, critically, that recomputing
// over unchanged data changes nothing.
//
// The scoping tests below exist because getting this wrong is not a subtle
// failure: judging a break over the whole window marked 53 % of 15m zones and
// 79 % of 4h zones broken on live data, because a level is a level precisely
// because price traded through that region before it was recognised.

#include "zone_lifecycle.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace alttech::modules::structure;

namespace {

SBar bar(int64_t minute, double low, double high, double close) {
    SBar b;
    b.open_ms = minute * 60'000;
    b.low = low;
    b.high = high;
    b.close = close;
    b.open = close;
    return b;
}

// A band around 100 with an ATR of 2, so a break needs a close beyond it by 1.0.
constexpr double kAtr = 2.0;

// Default: recognised before every bar in these fixtures, so the window is
// judged in full. Pass a minute to place the moment of recognition inside it.
ZoneBand band(std::string_view kind, int64_t created_minute = -1) {
    return ZoneBand{99.0, 100.0, 101.0, kind, created_minute * 60'000};
}

} // namespace

// --- scoping: the failure that reached production -------------------------

TEST_CASE("a bar older than the zone cannot break it") {
    // Price closed far below the band at minute 0, then the level was
    // recognised at minute 5. That history is why it is a level, not evidence
    // that it has failed.
    const std::vector<SBar> bars = {
        bar(0, 96.0, 98.0, 97.0),   // decisively below, BEFORE creation
        bar(6, 99.5, 101.0, 100.0), // back at the band, after creation
    };
    CHECK_FALSE(evaluate_zone(bars, 0, band("support", 5), kAtr).broken);
    // The same window, with the zone recognised before that bar, does break.
    CHECK(evaluate_zone(bars, 0, band("support", -1), kAtr).broken);
}

TEST_CASE("a bar after the zone was recognised does break it") {
    const std::vector<SBar> bars = {
        bar(0, 99.5, 101.0, 100.0),
        bar(6, 96.0, 98.0, 97.0), // 3.0 below the band, well past the 1.0 margin
    };
    CHECK(evaluate_zone(bars, 0, band("support", 5), kAtr).broken);
}

TEST_CASE("a freshly mirrored zone is not immediately broken") {
    // The flip case: resistance broke upward, so the same band becomes support
    // and is created at the newest bar. Price is above it, which is exactly the
    // situation a whole-window break test misread as an instant break — and
    // that cascade produced 561 broken zones in one minute on live data.
    const std::vector<SBar> bars = {
        bar(0, 96.0, 98.0, 97.0),     // long before: price was below
        bar(1, 99.0, 101.0, 100.0),
        bar(2, 101.5, 104.0, 103.5),  // the break that caused the flip
    };
    const auto mirror = evaluate_zone(bars, 0, band("support", 2), kAtr);
    CHECK_FALSE(mirror.broken);
}

TEST_CASE("touches are counted before creation, breaks are not") {
    // Asymmetric on purpose: how often the market respected a band is the
    // evidence that it is a level; whether it failed is only meaningful while
    // the level was being tracked.
    const std::vector<SBar> bars = {
        bar(0, 99.0, 101.0, 100.0), // touch, before creation
        bar(1, 96.0, 98.0, 97.0),   // break-shaped, before creation
        bar(6, 99.5, 100.5, 100.0), // touch, after creation
    };
    const auto state = evaluate_zone(bars, 0, band("support", 5), kAtr);
    CHECK(state.touch_count == 2);
    CHECK_FALSE(state.broken);
}

// --- purity and self-correction -------------------------------------------

TEST_CASE("evaluating the same window twice gives the same answer") {
    const std::vector<SBar> bars = {
        bar(0, 98.0, 102.0, 100.0),
        bar(1, 99.5, 101.5, 100.5),
        bar(2, 98.0, 100.0, 99.5),
    };
    const auto first = evaluate_zone(bars, 0, band("support"), kAtr);
    const auto second = evaluate_zone(bars, 0, band("support"), kAtr);
    const auto third = evaluate_zone(bars, 0, band("support"), kAtr);
    CHECK(first.touch_count == second.touch_count);
    CHECK(second.touch_count == third.touch_count);
    CHECK(first.reaction_count == second.reaction_count);
    CHECK(first.broken == second.broken);
    CHECK(first.last_touch_ms == second.last_touch_ms);
}

TEST_CASE("a corrected bar changes the verdict instead of preserving the old one") {
    std::vector<SBar> bars = {bar(0, 99.5, 101.0, 100.0), bar(1, 97.0, 100.0, 97.5)};
    CHECK(evaluate_zone(bars, 0, band("support"), kAtr).broken);

    // AltSpotter repairs that bar: the close was really 99.5, inside the band.
    bars[1] = bar(1, 99.0, 100.0, 99.5);
    CHECK_FALSE(evaluate_zone(bars, 0, band("support"), kAtr).broken);
}

// --- the break predicate itself -------------------------------------------

TEST_CASE("a wick through the band is a test, a close through it is a break") {
    const std::vector<SBar> wick = {bar(0, 95.0, 101.0, 100.0)};
    const auto tested = evaluate_zone(wick, 0, band("support"), kAtr);
    CHECK(tested.touch_count == 1);
    CHECK_FALSE(tested.broken);

    const std::vector<SBar> through = {bar(0, 95.0, 101.0, 97.0)};
    CHECK(evaluate_zone(through, 0, band("support"), kAtr).broken);
}

TEST_CASE("support and resistance break in opposite directions") {
    const std::vector<SBar> above = {bar(0, 100.0, 103.0, 102.5)};
    CHECK(evaluate_zone(above, 0, band("resistance"), kAtr).broken);
    CHECK_FALSE(evaluate_zone(above, 0, band("support"), kAtr).broken);

    const std::vector<SBar> below = {bar(0, 97.0, 100.0, 97.5)};
    CHECK(evaluate_zone(below, 0, band("support"), kAtr).broken);
    CHECK_FALSE(evaluate_zone(below, 0, band("resistance"), kAtr).broken);
}

TEST_CASE("a close just inside the margin does not break the band") {
    // 0.5 below the lower edge, against a 1.0 margin.
    const std::vector<SBar> shallow = {bar(0, 98.0, 100.0, 98.5)};
    CHECK_FALSE(evaluate_zone(shallow, 0, band("support"), kAtr).broken);
}

// --- creation guard --------------------------------------------------------

TEST_CASE("a band price has already left is refused rather than created") {
    // Measured on live data, 23-30 % of candidates sit beyond the current
    // close. Creating them only to break them next cycle is churn and false
    // events; they are not levels any more.
    CHECK(close_is_beyond(97.0, band("support"), kAtr));
    CHECK(close_is_beyond(103.0, band("resistance"), kAtr));
    CHECK_FALSE(close_is_beyond(100.0, band("support"), kAtr));
    CHECK_FALSE(close_is_beyond(100.0, band("resistance"), kAtr));
    // Just inside the margin is still a usable level.
    CHECK_FALSE(close_is_beyond(98.5, band("support"), kAtr));
}

// --- ageing out of the window ---------------------------------------------

TEST_CASE("a break too old to be revised is kept rather than re-derived away") {
    constexpr int64_t window_start = 100 * 60'000;
    constexpr int64_t old_zone = 10 * 60'000;   // created before the window
    constexpr int64_t new_zone = 150 * 60'000;  // created inside the window

    // Evidence outside the window cannot be disproved, so it stands.
    CHECK(retains_break(false, true, old_zone, window_start));
    // Evidence inside the window is re-derived, so a repair can clear it.
    CHECK_FALSE(retains_break(false, true, new_zone, window_start));
    // A fresh derivation always wins.
    CHECK(retains_break(true, false, new_zone, window_start));
    CHECK_FALSE(retains_break(false, false, old_zone, window_start));
}

// --- counters --------------------------------------------------------------

TEST_CASE("a reaction is a touch the price then rejected") {
    const std::vector<SBar> rejected = {bar(0, 99.0, 102.5, 102.0)};
    const auto state = evaluate_zone(rejected, 0, band("support"), kAtr);
    CHECK(state.touch_count == 1);
    CHECK(state.reaction_count == 1);

    const std::vector<SBar> flat = {bar(0, 99.0, 101.0, 100.0)};
    const auto weak = evaluate_zone(flat, 0, band("support"), kAtr);
    CHECK(weak.touch_count == 1);
    CHECK(weak.reaction_count == 0);
}

TEST_CASE("only the last bar makes a zone recently touched") {
    const std::vector<SBar> ended_away = {bar(0, 99.0, 101.0, 100.0), bar(1, 90.0, 91.0, 90.5)};
    CHECK(evaluate_zone(ended_away, 0, band("support"), kAtr).touch_count == 1);
    CHECK_FALSE(evaluate_zone(ended_away, 0, band("support"), kAtr).touched_recently);

    const std::vector<SBar> ended_on = {bar(0, 90.0, 91.0, 90.5), bar(1, 99.0, 101.0, 100.0)};
    CHECK(evaluate_zone(ended_on, 0, band("support"), kAtr).touched_recently);
}

TEST_CASE("an empty window yields no state rather than a fabricated one") {
    const std::vector<SBar> none;
    const auto state = evaluate_zone(none, 0, band("support"), kAtr);
    CHECK(state.touch_count == 0);
    CHECK(state.reaction_count == 0);
    CHECK_FALSE(state.broken);
    CHECK_FALSE(state.touched_recently);
    CHECK(state.last_touch_ms == 0);
}
