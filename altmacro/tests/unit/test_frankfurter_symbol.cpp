#include "altmacro/providers/frankfurter.hpp"

#include <catch2/catch_test_macros.hpp>

using altmacro::providers::FrankfurterProvider;

TEST_CASE("a symbol must name its authority", "[frankfurter]") {
    // Frankfurter's unfiltered endpoint returns a blend across 97 sources.
    // That blend is nobody's published rate, so a symbol that does not name a
    // provider is refused rather than defaulted.
    CHECK_FALSE(FrankfurterProvider::parse_symbol("CNY").has_value());
    CHECK_FALSE(FrankfurterProvider::parse_symbol("CFETS:CNY").has_value());
    CHECK_FALSE(FrankfurterProvider::parse_symbol(":USD:CNY").has_value());
}

TEST_CASE("PROVIDER:BASE:QUOTE parses", "[frankfurter]") {
    auto b = FrankfurterProvider::parse_symbol("CFETS:USD:CNY");
    REQUIRE(b.has_value());
    CHECK(b->source == "CFETS");
    CHECK(b->base == "USD");
    CHECK(b->quote == "CNY");
}

TEST_CASE("the ECB binding is a different series from the CFETS one", "[frankfurter][cny]") {
    // The PBOC's central parity and the ECB's reference rate are not two
    // sources for one number; they are two numbers.
    auto pboc = FrankfurterProvider::parse_symbol("CFETS:USD:CNY");
    auto ecb = FrankfurterProvider::parse_symbol("ECB:EUR:CNY");
    REQUIRE(pboc.has_value());
    REQUIRE(ecb.has_value());
    CHECK(pboc->source != ecb->source);
    CHECK(pboc->base != ecb->base);
}
