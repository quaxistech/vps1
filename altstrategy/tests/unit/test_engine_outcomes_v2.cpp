// Market-path outcome evaluator version 2 (QUALITY-04): every case resolves to
// a deterministic outcome or to an explicit uncertainty, never to the reading
// that flatters the plan.

#include "altstrategy/engine/outcomes.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <initializer_list>

using namespace altstrategy::engine;

namespace {

constexpr int64_t kMin = 60'000;
constexpr int64_t kT0 = 1'800'000'000'000;   // minute-aligned

PathBar bar(int64_t open_ms, double o, double h, double l, double c) { return PathBar{open_ms, o, h, l, c}; }

// Minute bars from kT0 + start_min, one per (o, h, l, c).
std::vector<PathBar> minutes(int start_min, std::initializer_list<std::array<double, 4>> ohlc) {
    std::vector<PathBar> out;
    int64_t t = kT0 + start_min * kMin;
    for (const auto& x : ohlc) {
        out.push_back(bar(t, x[0], x[1], x[2], x[3]));
        t += kMin;
    }
    return out;
}

// Flat minutes at `px` until `to_min` (exclusive), appended.
void flat_until(std::vector<PathBar>& bars, int to_min, double px) {
    int64_t t = bars.empty() ? kT0 : bars.back().open_ms + kMin;
    for (; t < kT0 + to_min * kMin; t += kMin) bars.push_back(bar(t, px, px, px, px));
}

PricePath candles(std::vector<PathBar> bars) {
    PricePath p;
    p.bars = std::move(bars);
    p.source = OutcomePath::m1_candles;
    p.bar_ms = kMin;
    return p;
}

// LONG at 100 by MARKET, stop 99, target 102, 30-minute time stop, 10 bps cost.
OutcomeGeometry long_market() {
    OutcomeGeometry g;
    g.side = Side::long_;
    g.entry_type = EntryType::market;
    g.entry_price = 100.0;
    g.stop_loss_price = 99.0;
    g.take_profit_price = 102.0;
    g.created_at_ms = kT0;
    g.entry_fill_timeout_ms = 5 * kMin;
    g.time_stop_at_ms = kT0 + 30 * kMin;
    g.cost_bps = 10.0;
    return g;
}

MarketOutcome eval(const OutcomeGeometry& g, const PricePath& p) {
    return reconstruct_outcome_v2(g, p, "cand-v2", "sig-v2", kT0 + 60 * kMin);
}

} // namespace

TEST_CASE("v2: target before stop on a complete path, with gross and net from the fill", "[outcomes][v2]") {
    auto bars = minutes(0, {{100, 100.5, 99.8, 100.4}, {100.4, 101.0, 100.2, 100.9}, {100.9, 102.5, 100.8, 102.2}});
    flat_until(bars, 40, 102.2);
    const auto o = eval(long_market(), candles(bars));
    CHECK(o.outcome == MarketOutcomeType::target_first);
    CHECK(o.resolution == "target");
    CHECK(o.evaluator_version == kOutcomeEvaluatorVersion);
    CHECK(o.entry_fill_price == Catch::Approx(100.0));
    CHECK(o.realized_gross_return == Catch::Approx(0.02));
    CHECK(o.realized_net_return == Catch::Approx(0.02 - 0.001));
    CHECK(o.holding_time_ms == 2 * kMin);
}

TEST_CASE("v2: stop before target, with the side's sign, and its mirror for a short", "[outcomes][v2]") {
    auto bars = minutes(0, {{100, 100.2, 99.9, 100.0}, {100.0, 100.1, 98.8, 99.0}});
    flat_until(bars, 40, 99.0);
    const auto lo = eval(long_market(), candles(bars));
    CHECK(lo.outcome == MarketOutcomeType::stop_first);
    CHECK(lo.realized_gross_return == Catch::Approx(-0.01));

    // The mirror: the same path reflected around 100 against a SHORT.
    auto mirrored = bars;
    for (auto& b : mirrored) {
        const double h = 200 - b.low, l = 200 - b.high;
        b = bar(b.open_ms, 200 - b.open, h, l, 200 - b.close);
    }
    auto g = long_market();
    g.side = Side::short_;
    g.stop_loss_price = 101.0;
    g.take_profit_price = 98.0;
    const auto so = eval(g, candles(mirrored));
    CHECK(so.outcome == MarketOutcomeType::stop_first);
    CHECK(so.realized_gross_return == Catch::Approx(lo.realized_gross_return));
    CHECK(so.mae == Catch::Approx(lo.mae));
    CHECK(so.mfe == Catch::Approx(lo.mfe));
}

TEST_CASE("v2: both barriers inside one bar is AMBIGUOUS at any resolution -- never ordered by a heuristic",
          "[outcomes][v2][regression]") {
    auto bars = minutes(0, {{100, 100.2, 99.9, 100.0}, {100.0, 102.4, 98.6, 101.0}});
    flat_until(bars, 40, 101.0);
    auto p = candles(bars);
    CHECK(eval(long_market(), p).outcome == MarketOutcomeType::ambiguous);
    // The same bar as a 1-second trade bucket: version 1 decided it by which
    // level was nearer the bucket's open; version 2 does not guess.
    p.source = OutcomePath::raw_trades;
    const auto o = eval(long_market(), p);
    CHECK(o.outcome == MarketOutcomeType::ambiguous);
    CHECK(o.resolution == "both_barriers_in_bar");
    CHECK(o.ambiguous);
    CHECK(reconstruct_outcome(long_market(), p.bars, OutcomePath::raw_trades, "c", "s", kT0).outcome !=
          MarketOutcomeType::ambiguous);   // what version 1 did
}

TEST_CASE("v2: a stop entry that gaps past its trigger fills at the open, not at the trigger",
          "[outcomes][v2][regression]") {
    auto g = long_market();
    g.entry_type = EntryType::stop_market;
    g.trigger_price = 100.5;
    g.entry_price = 100.5;
    auto bars = minutes(0, {{100, 100.3, 99.9, 100.2}, {100.9, 101.2, 100.8, 101.0}, {101.0, 102.6, 100.9, 102.4}});
    flat_until(bars, 40, 102.4);
    const auto o = eval(g, candles(bars));
    CHECK(o.entry_fill_price == Catch::Approx(100.9));      // the open it gapped to
    CHECK(o.outcome == MarketOutcomeType::target_first);
    CHECK(o.realized_gross_return == Catch::Approx((102.0 - 100.9) / 100.9));
    CHECK(reconstruct_outcome(g, bars, OutcomePath::m1_candles, "c", "s", kT0).entry_fill_price ==
          Catch::Approx(100.5));                            // version 1: the trigger
}

TEST_CASE("v2: a limit that is merely touched is not filled", "[outcomes][v2][regression]") {
    auto g = long_market();
    g.entry_type = EntryType::limit;
    g.entry_price = 99.5;
    auto bars = minutes(0, {{100, 100.2, 99.5, 99.8}, {99.8, 100.1, 99.5, 100.0}});
    flat_until(bars, 40, 100.0);
    const auto o = eval(g, candles(bars));
    CHECK(o.outcome == MarketOutcomeType::entry_not_reached);
    CHECK(o.resolution == "limit_touched_never_traded_through");
    // Traded THROUGH it: filled at the limit.
    bars[1] = bar(bars[1].open_ms, 99.8, 100.1, 99.4, 100.0);
    CHECK(eval(g, candles(bars)).entry_fill_price == Catch::Approx(99.5));
}

TEST_CASE("v2: a gap in the entry window is DATA_INCOMPLETE, not 'never filled'", "[outcomes][v2][regression]") {
    auto g = long_market();
    g.entry_type = EntryType::limit;
    g.entry_price = 99.0;
    auto bars = minutes(0, {{100, 100.2, 99.9, 100.0}});
    flat_until(bars, 40, 100.0);
    auto p = candles(bars);
    p.gaps.push_back({kT0 + 2 * kMin, kT0 + 3 * kMin});
    const auto o = eval(g, p);
    CHECK(o.outcome == MarketOutcomeType::data_incomplete);
    CHECK(o.resolution == "gap_in_entry_window");
}

TEST_CASE("v2: a gap after entry and before resolution is DATA_INCOMPLETE", "[outcomes][v2][regression]") {
    auto bars = minutes(0, {{100, 100.2, 99.9, 100.1}});
    flat_until(bars, 40, 100.1);
    bars[20] = bar(bars[20].open_ms, 100.1, 102.5, 100.0, 102.2);   // target, after the gap
    auto p = candles(bars);
    p.gaps.push_back({kT0 + 10 * kMin, kT0 + 11 * kMin});
    const auto o = eval(long_market(), p);
    CHECK(o.outcome == MarketOutcomeType::data_incomplete);
    CHECK(o.resolution == "gap_before_resolution");
    CHECK(o.path_gap_count == 1);
}

TEST_CASE("v2: a path that ends before the time stop is DATA_INCOMPLETE", "[outcomes][v2]") {
    auto bars = minutes(0, {{100, 100.2, 99.9, 100.1}});
    flat_until(bars, 20, 100.1);
    const auto o = eval(long_market(), candles(bars));
    CHECK(o.outcome == MarketOutcomeType::data_incomplete);
    CHECK(o.resolution == "path_ends_before_time_stop");
}

TEST_CASE("v2: the time stop exits at the close of the last bar that ENDED by it, net of costs",
          "[outcomes][v2]") {
    auto bars = minutes(0, {{100, 100.2, 99.9, 100.1}});
    flat_until(bars, 40, 100.1);
    bars[29] = bar(bars[29].open_ms, 100.1, 100.4, 100.0, 100.3);   // [29, 30) ends at the time stop
    bars[30] = bar(bars[30].open_ms, 100.3, 101.5, 100.3, 101.4);   // starts AT it: not part of the trade
    const auto o = eval(long_market(), candles(bars));
    CHECK(o.outcome == MarketOutcomeType::time_stop);
    CHECK(o.exit_price == Catch::Approx(100.3));
    CHECK(o.realized_net_return == Catch::Approx(0.003 - 0.001));
    CHECK(o.resolved_time_ms == kT0 + 30 * kMin);
}

TEST_CASE("v2: a fill already past the target is ENTRY_EXPIRED, with no return", "[outcomes][v2]") {
    auto bars = minutes(0, {{102.3, 102.6, 102.1, 102.5}});
    flat_until(bars, 40, 102.5);
    const auto o = eval(long_market(), candles(bars));
    CHECK(o.outcome == MarketOutcomeType::entry_expired);
    CHECK(o.resolution == "filled_beyond_level");
    CHECK(o.realized_net_return == 0.0);   // stored as NULL from version 2 (decision_store)
}

TEST_CASE("v2: TARGET_FIRST does not mean a profitable trade", "[outcomes][v2][economics]") {
    // A 5 bps target against a 10 bps cost: hit first, lost net.
    auto g = long_market();
    g.take_profit_price = 100.05;
    auto bars = minutes(0, {{100, 100.02, 99.98, 100.0}, {100.0, 100.08, 99.97, 100.06}});
    flat_until(bars, 40, 100.06);
    const auto o = eval(g, candles(bars));
    CHECK(o.outcome == MarketOutcomeType::target_first);
    CHECK(o.realized_gross_return > 0);
    CHECK(o.realized_net_return < 0);
}

TEST_CASE("v2: in the entry bar an exit counts only where the entry proves its order",
          "[outcomes][v2][regression]") {
    // A stop entry crossing its trigger upward: a target above it is provably
    // reached after the fill; a stop below it is not.
    auto g = long_market();
    g.entry_type = EntryType::stop_market;
    g.trigger_price = 100.5;
    g.entry_price = 100.5;
    auto bars = minutes(0, {{100, 102.5, 99.9, 102.2}});
    flat_until(bars, 40, 102.2);
    CHECK(eval(g, candles(bars)).outcome == MarketOutcomeType::target_first);
    bars[0] = bar(bars[0].open_ms, 100, 100.8, 98.7, 99.0);
    const auto stop_unknown = eval(g, candles(bars));
    CHECK(stop_unknown.outcome == MarketOutcomeType::ambiguous);
    CHECK(stop_unknown.resolution == "entry_bar_stop_order_unknown");

    // A limit traded through on the way down: the stop below is provably
    // after the fill, a target above is not.
    auto lim = long_market();
    lim.entry_type = EntryType::limit;
    lim.entry_price = 99.5;
    auto down = minutes(0, {{100, 100.1, 98.8, 99.0}});
    flat_until(down, 40, 99.0);
    CHECK(eval(lim, candles(down)).outcome == MarketOutcomeType::stop_first);
    down[0] = bar(down[0].open_ms, 100, 102.4, 99.4, 100.0);
    const auto target_unknown = eval(lim, candles(down));
    CHECK(target_unknown.outcome == MarketOutcomeType::ambiguous);
    CHECK(target_unknown.resolution == "entry_bar_target_order_unknown");
}

TEST_CASE("v2: a stop the market gaps through exits at the open it gapped to", "[outcomes][v2]") {
    auto bars = minutes(0, {{100, 100.2, 99.9, 100.0}, {98.5, 98.7, 98.2, 98.4}});
    flat_until(bars, 40, 98.4);
    const auto o = eval(long_market(), candles(bars));
    CHECK(o.outcome == MarketOutcomeType::stop_first);
    CHECK(o.resolution == "stop_gapped");
    CHECK(o.exit_price == Catch::Approx(98.5));
}

TEST_CASE("v2: MFE/MAE count only excursions provably after the fill", "[outcomes][v2]") {
    // A limit filled on the way down: the entry bar's earlier high is not an
    // excursion of the position.
    auto lim = long_market();
    lim.entry_type = EntryType::limit;
    lim.entry_price = 99.5;
    lim.take_profit_price = 105.0;
    auto bars = minutes(0, {{100, 101.5, 99.4, 99.6}});
    flat_until(bars, 40, 99.6);
    const auto o = eval(lim, candles(bars));
    CHECK(o.outcome == MarketOutcomeType::time_stop);
    CHECK(o.mfe == Catch::Approx((99.6 - 99.5) / 99.5));
    CHECK(o.mae == Catch::Approx((99.5 - 99.4) / 99.5));
}

TEST_CASE("v2: the same immutable inputs give the same outcome", "[outcomes][v2]") {
    auto bars = minutes(0, {{100, 100.5, 99.8, 100.4}, {100.4, 102.5, 100.2, 102.2}});
    flat_until(bars, 40, 102.2);
    const auto p = candles(bars);
    const auto a = eval(long_market(), p);
    const auto b = eval(long_market(), p);
    CHECK(a.outcome_id == b.outcome_id);
    CHECK(a.outcome == b.outcome);
    CHECK(a.realized_net_return == b.realized_net_return);
    CHECK(a.resolution == b.resolution);
}

namespace {

// One-second trade buckets at the given seconds after kT0, all at `px`, over
// a range the source vouches for up to `covered_min` minutes after kT0.
PricePath quiet_tape(std::initializer_list<int64_t> seconds, double px, int covered_min) {
    PricePath p;
    for (const auto s : seconds) p.bars.push_back(bar(kT0 + s * 1000, px, px, px, px));
    p.source = OutcomePath::raw_trades;
    p.bar_ms = 1000;
    p.covered_until_ms = kT0 + covered_min * kMin;
    return p;
}

} // namespace

TEST_CASE("v2: a market order on a quiet tape fills at the first print, however late (HISTORY-10)",
          "[outcomes][v2][regression]") {
    // Nothing prints for three minutes after the decision. Version 1 called
    // this ENTRY_NOT_REACHED -- for an order that takes the book.
    const auto o = eval(long_market(), quiet_tape({180, 600, 1700}, 100.2, 31));
    CHECK(o.outcome == MarketOutcomeType::time_stop);
    CHECK(o.entry_time_ms == kT0 + 180'000);
    CHECK(o.entry_fill_price == Catch::Approx(100.2));
    // No print at all before the time stop: unobserved, not "never filled".
    const auto none = eval(long_market(), quiet_tape({1900}, 100.2, 31));
    CHECK(none.outcome == MarketOutcomeType::data_incomplete);
    CHECK(none.resolution == "no_print_before_time_stop");
}

TEST_CASE("v2: a quiet tail is covered when the source vouches for the range", "[outcomes][v2][regression]") {
    // The last trade is five minutes before the 30-minute time stop.
    auto p = quiet_tape({5, 400, 1500}, 100.3, 31);
    const auto o = eval(long_market(), p);
    CHECK(o.outcome == MarketOutcomeType::time_stop);
    CHECK(o.exit_price == Catch::Approx(100.3));   // the last price observable at the time stop
    CHECK(o.resolved_time_ms == kT0 + 30 * kMin);
    CHECK(o.holding_time_ms == 30 * kMin - 5'000);
    // Without the source's word the same bars are a path that ended early.
    p.covered_until_ms = 0;
    CHECK(eval(long_market(), p).resolution == "path_ends_before_time_stop");
}
