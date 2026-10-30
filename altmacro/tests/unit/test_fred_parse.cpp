#include "altmacro/providers/fred.hpp"

#include <catch2/catch_test_macros.hpp>

using altmacro::providers::FredProvider;

TEST_CASE("date-only ALFRED vintage waits for the full Eastern date", "[fred][vintage]") {
    const auto winter = FredProvider::date_only_vintage_bound("2026-01-05");
    const auto summer = FredProvider::date_only_vintage_bound("2026-07-05");
    REQUIRE(winter);
    REQUIRE(summer);
    constexpr int64_t day = 86'400'000;
    CHECK(*winter % day == 5 * 3'600'000);
    CHECK(*summer % day == 5 * 3'600'000); // safely after 04UTC summer day-end
    CHECK_FALSE(FredProvider::date_only_vintage_bound("2026-02-30"));
    CHECK_FALSE(FredProvider::date_only_vintage_bound("2026-aa-01"));
    CHECK_FALSE(FredProvider::date_only_vintage_bound("2026-13-01"));
    CHECK(FredProvider::date_only_vintage_bound("2024-02-29"));
    CHECK_FALSE(FredProvider::date_only_vintage_bound("2026-02-29"));
}

TEST_CASE("a normal FRED series parses", "[fred]") {
    const std::string body =
        "observation_date,DGS10\n"
        "2026-09-07,4.79\n"
        "2026-09-08,4.80\n";
    std::string column;
    auto points = FredProvider::parse_csv(body, &column);
    REQUIRE(points.has_value());
    CHECK(column == "DGS10");
    REQUIRE(points->size() == 2);
    CHECK((*points)[0].date == "2026-09-07");
    CHECK((*points)[0].value.value() == 4.79);
}

TEST_CASE("'.' becomes an absence, never zero", "[fred]") {
    // A yield series that stored "." as 0.0 would describe a market event that
    // never happened, and would do so plausibly.
    const std::string body =
        "observation_date,DGS10\n"
        "2026-09-07,.\n"
        "2026-09-08,4.80\n";
    std::string column;
    auto points = FredProvider::parse_csv(body, &column);
    REQUIRE(points.has_value());
    REQUIRE(points->size() == 2);
    CHECK_FALSE((*points)[0].value.has_value());
    CHECK((*points)[1].value.value() == 4.80);
}

TEST_CASE("a negative value parses", "[fred]") {
    const std::string body = "observation_date,DFII10\n2020-08-06,-1.08\n";
    std::string column;
    auto points = FredProvider::parse_csv(body, &column);
    REQUIRE(points.has_value());
    CHECK((*points)[0].value.value() == -1.08);
}

TEST_CASE("the ALFRED vintage column is exposed for checking", "[fred][vintage]") {
    // The column name is how a caller proves the server honoured the vintage
    // rather than quietly serving today's revision under a past date.
    const std::string body = "observation_date,DGS10_20260601\n1962-01-02,4.06\n";
    std::string column;
    auto points = FredProvider::parse_csv(body, &column);
    REQUIRE(points.has_value());
    CHECK(column == "DGS10_20260601");
}

TEST_CASE("a non-numeric value is refused, not coerced", "[fred]") {
    const std::string body = "observation_date,DGS10\n2026-09-07,n/a\n";
    std::string column;
    auto points = FredProvider::parse_csv(body, &column);
    CHECK_FALSE(points.has_value());
}

TEST_CASE("an empty body is refused", "[fred]") {
    std::string column;
    CHECK_FALSE(FredProvider::parse_csv("", &column).has_value());
}

TEST_CASE("a malformed row is refused", "[fred]") {
    const std::string body = "observation_date,DGS10\n2026-09-07\n";
    std::string column;
    CHECK_FALSE(FredProvider::parse_csv(body, &column).has_value());
}
