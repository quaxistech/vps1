// Local order-book reconstruction tests: per-venue snapshot/delta sequencing,
// gap/checksum desync detection, crossed-book guard, zero-size deletion.

#include "exchanges/order_book.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace altspotter;
using namespace altspotter::exchanges;

namespace {

Level lvl(const char* px, const char* sz) {
    Level l;
    l.price_str = px;
    l.size_str = sz;
    l.price = *Decimal::parse(px);
    l.size = *Decimal::parse(sz);
    return l;
}

BookUpdate snapshot(std::vector<Level> bids, std::vector<Level> asks, int64_t seq) {
    BookUpdate u;
    u.is_snapshot = true;
    u.bids = std::move(bids);
    u.asks = std::move(asks);
    u.sequence = seq;
    return u;
}

} // namespace

TEST_CASE("bybit snapshot then delta with zero-size delete", "[book][bybit]") {
    LocalBook book(LocalBook::Sequencing::bybit);
    REQUIRE(book.apply(snapshot({lvl("100", "1"), lvl("99", "2")},
                                {lvl("101", "1"), lvl("102", "3")}, 10)) ==
            BookApplyResult::reset);
    CHECK(book.bid_levels() == 2);

    BookUpdate d;
    d.sequence = 11;
    d.bids = {lvl("99", "0")};       // delete the 99 level
    d.asks = {lvl("101", "5")};      // update the 101 level
    CHECK(book.apply(d) == BookApplyResult::ok);
    CHECK(book.bid_levels() == 1);
    auto bids = book.top_bids(5);
    REQUIRE(bids.size() == 1);
    CHECK(bids[0].price.to_string() == "100");
    auto asks = book.top_asks(5);
    CHECK(asks[0].size.to_string() == "5");
}

TEST_CASE("bybit u==1 forces a snapshot reset", "[book][bybit]") {
    LocalBook book(LocalBook::Sequencing::bybit);
    book.apply(snapshot({lvl("100", "1")}, {lvl("101", "1")}, 50));
    BookUpdate d;               // service restart delivers a fresh snapshot at u=1
    d.sequence = 1;
    d.bids = {lvl("200", "1")};
    d.asks = {lvl("201", "1")};
    // sequence 1 <= last (50) but a non-snapshot regression => desync
    CHECK(book.apply(d) == BookApplyResult::desync_gap);
    // the runtime then receives the actual snapshot marked is_snapshot
    d.is_snapshot = true;
    CHECK(book.apply(d) == BookApplyResult::reset);
    CHECK(book.last_sequence() == 1);
}

TEST_CASE("binance futures pu chaining detects a gap", "[book][binance]") {
    LocalBook book(LocalBook::Sequencing::binance_futures);
    // REST snapshot seeds lastUpdateId = 100
    book.reset_from_rest({lvl("100", "1")}, {lvl("101", "1")}, 100, 0);

    // First delta must straddle lastUpdateId: U<=100<=u.
    BookUpdate d1;
    d1.first_update_id = 90;
    d1.sequence = 110;
    d1.prev_sequence = 89;
    d1.bids = {lvl("100", "2")};
    CHECK(book.apply(d1) == BookApplyResult::ok);
    CHECK(book.last_sequence() == 110);

    // Next delta with pu != previous u (110) is a gap.
    BookUpdate d2;
    d2.first_update_id = 111;
    d2.sequence = 120;
    d2.prev_sequence = 115; // != 110
    CHECK(book.apply(d2) == BookApplyResult::desync_gap);
    CHECK_FALSE(book.synced());
}

TEST_CASE("okx prevSeqId chaining + crossed-book guard", "[book][okx]") {
    LocalBook book(LocalBook::Sequencing::okx);
    BookUpdate snap;
    snap.is_snapshot = true;
    snap.sequence = 10;
    snap.bids = {lvl("100", "1")};
    snap.asks = {lvl("101", "1")};
    CHECK(book.apply(snap) == BookApplyResult::reset);

    BookUpdate d;
    d.sequence = 11;
    d.prev_sequence = 10;         // chains correctly
    d.asks = {lvl("101", "0")};   // remove the only ask
    d.bids = {lvl("102", "1")};   // new bid above nothing -> not crossed yet
    CHECK(book.apply(d) == BookApplyResult::empty);
    CHECK_FALSE(book.synced());
    book.apply(snapshot({lvl("100", "1")}, {lvl("101", "1")}, 11));

    BookUpdate gap;
    gap.sequence = 20;
    gap.prev_sequence = 15;       // != last (11) -> desync
    CHECK(book.apply(gap) == BookApplyResult::desync_gap);
}

TEST_CASE("okx checksum mismatch triggers desync", "[book][okx]") {
    LocalBook book(LocalBook::Sequencing::okx);
    BookUpdate snap;
    snap.is_snapshot = true;
    snap.sequence = 1;
    snap.bids = {lvl("100", "1")};
    snap.asks = {lvl("101", "1")};
    snap.has_checksum = true;
    snap.checksum = 12345; // deliberately wrong
    CHECK(book.apply(snap) == BookApplyResult::desync_checksum);
    CHECK_FALSE(book.synced());
}

TEST_CASE("crossed book is not published as valid", "[book]") {
    LocalBook book(LocalBook::Sequencing::bybit);
    book.apply(snapshot({lvl("100", "1")}, {lvl("101", "1")}, 1));
    BookUpdate d;
    d.sequence = 2;
    d.bids = {lvl("102", "1")};  // bid now above ask -> crossed
    CHECK(book.apply(d) == BookApplyResult::crossed);
    CHECK(book.crossed());
    CHECK_FALSE(book.synced());
    CHECK_FALSE(book.last_ofi());
}

TEST_CASE("genuine OFI measures queue changes and never snapshot displacement", "[book][ofi]") {
    LocalBook book(LocalBook::Sequencing::bybit);
    REQUIRE(book.apply(snapshot({lvl("100", "2")}, {lvl("102", "3")}, 1)) == BookApplyResult::reset);
    CHECK_FALSE(book.last_ofi());
    const auto epoch = book.epoch();
    BookUpdate d;
    d.sequence = 2;
    d.bids = {lvl("100", "5")};
    d.asks = {lvl("102", "5")};
    REQUIRE(book.apply(d) == BookApplyResult::ok);
    REQUIRE(book.last_ofi());
    CHECK(book.last_ofi()->to_string() == "1"); // +3 bid -2 ask
    CHECK(book.delta_counter() == 1);
    CHECK(book.epoch() == epoch);
    d.sequence = 3;
    d.bids = {lvl("101", "4")};
    d.asks = {};
    REQUIRE(book.apply(d) == BookApplyResult::ok);
    REQUIRE(book.last_ofi());
    CHECK(book.last_ofi()->to_string() == "4"); // improved bid, new queue
    d.sequence = 4;
    d.bids = {lvl("101", "0")};
    d.asks = {lvl("102", "0"), lvl("103", "7")};
    REQUIRE(book.apply(d) == BookApplyResult::ok);
    REQUIRE(book.last_ofi());
    CHECK(book.last_ofi()->to_string() == "1"); // -4 removed bid +5 removed ask
    d.sequence = 5;
    d.bids = {};
    d.asks = {lvl("102", "6")};
    REQUIRE(book.apply(d) == BookApplyResult::ok);
    REQUIRE(book.last_ofi());
    CHECK(book.last_ofi()->to_string() == "-6"); // improved ask, new sell queue
    d.sequence = 6;
    d.asks = {lvl("102", "3")};
    REQUIRE(book.apply(d) == BookApplyResult::ok);
    REQUIRE(book.last_ofi());
    CHECK(book.last_ofi()->to_string() == "3"); // removal; trade/cancel cause stays unknown
    REQUIRE(book.apply(snapshot({lvl("50", "100")}, {lvl("51", "200")}, 1)) == BookApplyResult::reset);
    CHECK_FALSE(book.last_ofi());
    CHECK(book.epoch() > epoch);
    CHECK(book.delta_counter() == 0);
}

TEST_CASE("OFI unavailable after sequence gap, reset or missing side", "[book][ofi]") {
    LocalBook book(LocalBook::Sequencing::okx);
    book.apply(snapshot({lvl("100", "1")}, {lvl("101", "1")}, 10));
    BookUpdate d;
    d.sequence = 11; d.prev_sequence = 10; d.bids = {lvl("100", "2")};
    REQUIRE(book.apply(d) == BookApplyResult::ok);
    REQUIRE(book.last_ofi());
    d.sequence = 12; d.prev_sequence = 9;
    REQUIRE(book.apply(d) == BookApplyResult::desync_gap);
    CHECK_FALSE(book.last_ofi());
    book.apply(snapshot({lvl("100", "1")}, {lvl("101", "1")}, 20));
    d.sequence = 21; d.prev_sequence = 20; d.bids = {}; d.asks = {lvl("101", "0")};
    REQUIRE(book.apply(d) == BookApplyResult::empty);
    CHECK_FALSE(book.last_ofi());
    CHECK_FALSE(book.synced());
    d.sequence = 22; d.prev_sequence = 21; d.asks = {lvl("102", "1")};
    CHECK(book.apply(d) == BookApplyResult::waiting);
}

TEST_CASE("invalid or empty books require a fresh valid snapshot before OFI", "[book][ofi]") {
    LocalBook book(LocalBook::Sequencing::binance_futures);
    book.reset_from_rest({}, {lvl("101", "1")}, 100, 0);
    CHECK_FALSE(book.synced());
    book.reset_from_rest({lvl("100", "1")}, {lvl("99", "1")}, 100, 0);
    CHECK_FALSE(book.synced());
    REQUIRE(book.apply(snapshot({lvl("100", "1")}, {lvl("101", "1")}, 100)) == BookApplyResult::reset);
    BookUpdate bad;
    bad.sequence = 101; bad.prev_sequence = 100; bad.bids = {lvl("100", "-1")};
    CHECK(book.apply(bad) == BookApplyResult::invalid);
    CHECK_FALSE(book.synced());
    CHECK_FALSE(book.last_ofi());
    REQUIRE(book.apply(snapshot({lvl("0", "1")}, {lvl("101", "1")}, 102)) == BookApplyResult::invalid);
}
