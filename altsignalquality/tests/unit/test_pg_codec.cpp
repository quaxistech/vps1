// The feature vector round-trips through PostgreSQL as a text literal. If a
// value changes between write and read, the model is trained on numbers that
// differ from the ones inference sees — silently, and only for some values.

#include "altsignalquality/data/pg_array.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <limits>
#include <random>

using namespace altsignalquality::pgarray;

TEST_CASE("floats round-trip exactly, including the awkward ones") {
    std::vector<float> values = {0.0F,
                                 -0.0F,
                                 1.0F,
                                 -1.5F,
                                 3.14159265F,
                                 1e-38F,
                                 3.4e38F,
                                 -3.4e38F,
                                 0.1F,
                                 1.0F / 3.0F};
    std::mt19937 rng(99);
    std::uniform_real_distribution<float> dist(-1e6F, 1e6F);
    for (int i = 0; i < 200; ++i) values.push_back(dist(rng));

    const auto parsed = parse_float_array(float_array_literal(values));
    REQUIRE(parsed.size() == values.size());
    for (size_t i = 0; i < values.size(); ++i) {
        INFO("index " << i << " value " << values[i]);
        REQUIRE(parsed[i] == values[i]);
    }
}

TEST_CASE("NaN survives the round trip as NaN, not as zero") {
    // A missing feature is NaN. Turning it into 0 on the way to the database
    // would hand LightGBM a real value where "absent" was meant.
    const std::vector<float> values = {1.0F, std::numeric_limits<float>::quiet_NaN(), 2.0F};
    const std::string literal = float_array_literal(values);
    REQUIRE(literal.find("NaN") != std::string::npos);
    const auto parsed = parse_float_array(literal);
    REQUIRE(parsed.size() == 3);
    REQUIRE(parsed[0] == 1.0F);
    REQUIRE(std::isnan(parsed[1]));
    REQUIRE(parsed[2] == 2.0F);
}

TEST_CASE("a SQL NULL element reads back as NaN") {
    const auto parsed = parse_float_array("{1,NULL,3}");
    REQUIRE(parsed.size() == 3);
    REQUIRE(std::isnan(parsed[1]));
}

TEST_CASE("an empty array is empty, and a malformed one is rejected") {
    REQUIRE(parse_float_array("{}").empty());
    REQUIRE_THROWS_AS(parse_float_array("1,2,3"), std::invalid_argument);
    REQUIRE_THROWS_AS(parse_float_array(""), std::invalid_argument);
}

TEST_CASE("the validity bitmap round-trips byte for byte") {
    std::vector<uint8_t> bitmap;
    for (int i = 0; i < 32; ++i) bitmap.push_back(static_cast<uint8_t>(i * 7 + 3));
    const auto literal = bytea_hex_literal(bitmap);
    REQUIRE(literal.rfind("\\x", 0) == 0);
    REQUIRE(parse_hex_bytea(literal) == bitmap);
    REQUIRE(parse_hex_bytea("\\x").empty());
    REQUIRE_THROWS_AS(parse_hex_bytea("0aff"), std::invalid_argument);
    REQUIRE_THROWS_AS(parse_hex_bytea("\\x0a0"), std::invalid_argument);
    REQUIRE_THROWS_AS(parse_hex_bytea("\\x0zg0"), std::invalid_argument);
}

TEST_CASE("reason codes are quoted and escaped so a comma cannot split one") {
    const std::vector<std::string> codes = {"SCORE_BELOW_THRESHOLD", "a,b", "quote\"inside",
                                            "back\\slash"};
    const std::string literal = text_array_literal(codes);
    REQUIRE(literal.front() == '{');
    REQUIRE(literal.back() == '}');
    REQUIRE(literal.find("\\\"inside") != std::string::npos);
    REQUIRE(literal.find("\\\\slash") != std::string::npos);
    REQUIRE(text_array_literal({}) == "{}");
}

// -----------------------------------------------------------------------------
// Defect: the codec was locale-dependent and threw on values it could itself
// produce.
//
// `snprintf("%.9g")` and `std::stof` both honour the process locale, so under a
// decimal-comma locale the writer would emit `1,5` — which PostgreSQL reads as
// TWO array elements — and the reader would stop at the comma. `std::stof` also
// throws `out_of_range` on a subnormal float, turning a routine dataset read on
// the training path into an exception. Both directions now use <charconv>,
// which is locale-independent by definition.
// -----------------------------------------------------------------------------
TEST_CASE("subnormal and extreme floats round-trip without throwing") {
    const std::vector<float> awkward = {
        std::numeric_limits<float>::min(),            // smallest normal
        std::numeric_limits<float>::denorm_min(),     // smallest subnormal
        std::numeric_limits<float>::min() / 2.0F,     // subnormal
        std::numeric_limits<float>::max(),
        -std::numeric_limits<float>::max(),
        1e-38F, -1e-38F, 1e38F, -1e38F};
    std::string literal;
    REQUIRE_NOTHROW(literal = float_array_literal(awkward));
    std::vector<float> parsed;
    REQUIRE_NOTHROW(parsed = parse_float_array(literal));
    REQUIRE(parsed.size() == awkward.size());
    for (size_t i = 0; i < awkward.size(); ++i) {
        INFO("index " << i);
        REQUIRE(parsed[i] == awkward[i]);
    }
}

TEST_CASE("the codec does not depend on the process locale") {
    // If the writer honoured a decimal-comma locale it would emit "1,5", which
    // PostgreSQL would read as two elements. Setting the locale is only
    // possible where the environment provides it; the assertion that always
    // holds is that the output contains no comma other than the separators.
    const std::vector<float> values = {1.5F, -2.25F, 0.125F};
    const std::string literal = float_array_literal(values);
    REQUIRE(std::count(literal.begin(), literal.end(), ',') == 2);
    REQUIRE(literal.find("1.5") != std::string::npos);
    const auto parsed = parse_float_array(literal);
    REQUIRE(parsed.size() == 3);
    REQUIRE(parsed[0] == 1.5F);
}

TEST_CASE("an infinity is refused rather than persisted") {
    // The assembler guards every denominator, so an infinity here means a bug
    // upstream. Writing it would hand the model a value no split can be
    // reasoned about.
    const std::vector<float> values = {1.0F, std::numeric_limits<float>::infinity()};
    REQUIRE_THROWS_AS(float_array_literal(values), std::invalid_argument);
}

TEST_CASE("a malformed array element is refused, not silently zeroed") {
    REQUIRE_THROWS_AS(parse_float_array("{1.0,not_a_number,2.0}"), std::invalid_argument);
    REQUIRE_THROWS_AS(parse_float_array("{1.0,1.5x,2.0}"), std::invalid_argument);
}
