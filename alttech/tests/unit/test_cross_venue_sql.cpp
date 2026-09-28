// The cross-venue sweep's market-table reads must constrain the leading column
// of the tables' primary key. Without it every read was a parallel sequential
// scan of a whole month partition (358 ms against 0.26 ms, measured on VPS1),
// the 27-pair sweep took ~140 s, and because the sweep computes the newest
// uncomputed bar first, 57% of 1m bars never got a cross-venue vector.

#include "cross_venue_sql.hpp"
#include "regime_readiness.hpp"

#include <catch2/catch_test_macros.hpp>

#include <regex>
#include <string>

using namespace alttech;
using namespace alttech::crossvenue;

namespace {

// Highest $N placeholder in a statement.
int max_placeholder(const std::string& sql) {
    int best = 0;
    const std::regex p(R"(\$(\d+))");
    for (auto it = std::sregex_iterator(sql.begin(), sql.end(), p); it != std::sregex_iterator();
         ++it) {
        best = std::max(best, std::stoi((*it)[1]));
    }
    return best;
}

} // namespace

TEST_CASE("the venue list is the cross-venue model's own venues", "[cross_venue][sql]") {
    REQUIRE(sql::venue_id_array() == "{1,2,3,4}");
    REQUIRE(kAllVenues.size() == 4);
}

TEST_CASE("every market-table read binds the venue list as the key's leading column",
          "[cross_venue][sql][regression]") {
    const std::regex bound(R"(exchange_id = ANY\(\$(\d+)::smallint\[\]\))");
    for (const char* statement : sql::kMarketReads) {
        const std::string text(statement);
        CAPTURE(text);
        std::smatch m;
        REQUIRE(std::regex_search(text, m, bound));
        // The call sites pass the venue array LAST; a statement whose venue
        // placeholder is not its highest-numbered one would bind a pair id or a
        // timestamp as the venue list.
        REQUIRE(std::stoi(m[1]) == max_placeholder(text));
        // And it is the leading column of what is filtered, not an afterthought
        // outside the WHERE clause.
        REQUIRE(text.find("WHERE exchange_id = ANY(") != std::string::npos);
    }
}

TEST_CASE("a fresh bar waits for all four venue regimes, briefly", "[cross_venue][regime][regression]") {
    // Computed before venue_lite had written the bar's 1m regimes, a bar got no
    // global regime, permanently: 297 of 675 bars once the sweep kept up.
    constexpr int64_t kBar = 1'000'000'020'000LL - 20'000;   // a minute's open
    constexpr int64_t kClose = kBar + 60'000;
    // Just closed, no regime yet: wait for the next pass.
    CHECK(wait_for_venue_regimes(0, kBar, kClose + 5'000));
    // Three of four: still worth waiting for the vote to be complete.
    CHECK(wait_for_venue_regimes(3, kBar, kClose + 30'000));
    // All four present: compute now.
    CHECK_FALSE(wait_for_venue_regimes(4, kBar, kClose + 5'000));
    // A venue that never delivers must not hold the bar past the bound...
    CHECK_FALSE(wait_for_venue_regimes(3, kBar, kClose + kRegimeWaitMs));
    CHECK_FALSE(wait_for_venue_regimes(0, kBar, kClose + kRegimeWaitMs + 1));
    // ...and a backfilled bar is computed with what exists, as before.
    CHECK_FALSE(wait_for_venue_regimes(0, kBar, kClose + 3'600'000));
}
