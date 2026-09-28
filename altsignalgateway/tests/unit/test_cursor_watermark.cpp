// Regression: the durable client cursor is a CONTIGUOUS watermark.
//
// THE DEFECT THIS FILE EXISTS FOR. The first implementation advanced the cursor
// with `GREATEST(cursor, acked_sequence)` on every individual acknowledgement.
// A client that acknowledges in batches — receive four trades, commit them,
// acknowledge the fourth — moved the cursor to the fourth, and the three in
// between fell below it. On the next RESUME they were never delivered again.
//
// Reproduced live against the deployed service: sequences 537..540 delivered,
// only 540 acknowledged, cursor became 540, and 538 and 539 sat in JOURNALED
// with no client that would ever be offered them. Two trades lost, under a
// perfectly well-behaved consumer.
//
// The rule below is the fix, extracted so it can be tested without a socket:
// the cursor is the highest sequence under which the client is owed NOTHING —
// everything at or below it was acknowledged, or was never offered at all.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <set>

namespace {

// The exact expression Session::maybe_advance_cursor evaluates.
int64_t watermark(const std::set<int64_t>& outstanding, int64_t examined_to) {
    return outstanding.empty() ? examined_to : (*outstanding.begin() - 1);
}

// A miniature of the session's cursor bookkeeping.
struct Cursor {
    std::set<int64_t> outstanding;
    int64_t examined_to = 0;
    int64_t submitted = 0;

    void offer(std::initializer_list<int64_t> seqs, int64_t scanned_to) {
        for (auto s : seqs) outstanding.insert(s);
        if (scanned_to > examined_to) examined_to = scanned_to;
        advance();
    }
    void ack(int64_t seq) {
        outstanding.erase(seq);
        advance();
    }
    void advance() {
        const int64_t w = watermark(outstanding, examined_to);
        if (w > submitted) submitted = w;
    }
};

} // namespace

TEST_CASE("a batch acknowledgement never strands the messages before it") {
    Cursor c;
    c.submitted = 536;
    c.examined_to = 536;
    c.offer({537, 538, 539, 540}, 540);
    CHECK(c.submitted == 536);   // nothing acknowledged yet, nothing settled

    // The exact live reproduction: only the last of the batch is acknowledged.
    // Under the defect the cursor became 540 here and 537..539 were lost.
    c.ack(540);
    CHECK(c.submitted == 536);   // 537 is still the smallest owed — nothing settles

    c.ack(537);
    CHECK(c.submitted == 537);   // the gap closed from below, by exactly one
    c.ack(538);
    CHECK(c.submitted == 538);
    c.ack(539);
    CHECK(c.submitted == 540);   // 540 was already acknowledged, so the run collapses
}

TEST_CASE("in-order acknowledgement advances one at a time") {
    Cursor c;
    c.offer({1, 2, 3}, 3);
    c.ack(1);
    CHECK(c.submitted == 1);
    c.ack(2);
    CHECK(c.submitted == 2);
    c.ack(3);
    CHECK(c.submitted == 3);
}

TEST_CASE("out-of-order acknowledgement never overtakes a gap") {
    Cursor c;
    c.offer({10, 11, 12, 13}, 13);
    CHECK(c.submitted == 9);     // 1..9 were examined and never offered
    c.ack(13);
    c.ack(12);
    c.ack(11);
    CHECK(c.submitted == 9);     // 10 is still outstanding; nothing above it settles
    c.ack(10);
    CHECK(c.submitted == 13);    // and now the whole run does
}

TEST_CASE("sequences that were never offered do not hold the cursor back") {
    // The pump skips expired signals. The client is never told about them, so
    // the cursor must be free to move over them — otherwise a long run of
    // expired signals would pin a consumer's cursor forever and every reconnect
    // would rescan them.
    Cursor c;
    c.offer({}, 100);            // 1..100 examined, all expired, none offered
    CHECK(c.submitted == 100);

    c.offer({101}, 101);
    CHECK(c.submitted == 100);   // 101 was offered and is outstanding
    c.ack(101);
    CHECK(c.submitted == 101);
}

TEST_CASE("a mixture of expired and offered settles correctly") {
    Cursor c;
    // The pump examined 1..10 and offered only 4 and 9; the rest had expired.
    c.offer({4, 9}, 10);
    CHECK(c.submitted == 3);     // everything below the smallest offered is settled
    c.ack(9);
    CHECK(c.submitted == 3);     // 4 still outstanding
    c.ack(4);
    CHECK(c.submitted == 10);    // now the whole examined window is settled
}

TEST_CASE("a dropped message keeps the cursor below it") {
    // A frame that never reached the socket stays in the outstanding set on
    // purpose: the client did not see it, so the cursor must not pass it.
    Cursor c;
    c.offer({20, 21, 22}, 22);
    c.ack(20);
    CHECK(c.submitted == 20);
    // 21 was dropped by the outbound bound and is NOT erased; 22 is acked.
    c.ack(22);
    CHECK(c.submitted == 20);    // 21 pins the cursor, and RESUME will redeliver it
}

TEST_CASE("the cursor never moves backwards") {
    Cursor c;
    c.offer({5}, 5);
    c.ack(5);
    CHECK(c.submitted == 5);
    // A later window that examines less than what is already settled cannot
    // pull the cursor down.
    c.examined_to = 2;
    c.advance();
    CHECK(c.submitted == 5);
}

TEST_CASE("the watermark expression itself") {
    CHECK(watermark({}, 0) == 0);
    CHECK(watermark({}, 42) == 42);
    CHECK(watermark({1}, 10) == 0);
    CHECK(watermark({7, 8, 9}, 100) == 6);
    // The smallest outstanding sequence is the only one that matters.
    CHECK(watermark({9, 8, 7}, 100) == 6);
}
