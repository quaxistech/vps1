// The label spec decides what the model is optimising. These tests pin the two
// exclusions that make the difference between measuring signal quality and
// measuring something else entirely.

#include "altsignalquality/domain/labeling.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace altsignalquality::domain;

namespace {
OutcomeRecord rec(std::string categorical, double net_return, bool usable = true) {
    OutcomeRecord o;
    o.categorical = std::move(categorical);
    o.net_return_after_costs = net_return;
    o.binary_label_usable = usable;
    o.net_return_present = true;
    return o;
}
} // namespace

TEST_CASE("the label is economic, not categorical") {
    // A target that was eaten by costs is not a win, and a time-stop that closed
    // in profit is not a loss. Anything else optimises the wrong quantity.
    REQUIRE(label_of(rec("TARGET_FIRST", 0.004)).label == 1);
    REQUIRE(label_of(rec("TARGET_FIRST", -0.0001)).label == 0);
    REQUIRE(label_of(rec("STOP_FIRST", -0.002)).label == 0);
    REQUIRE(label_of(rec("TIME_STOP", 0.0007)).label == 1);
    REQUIRE(label_of(rec("TIME_STOP", -0.0007)).label == 0);
}

TEST_CASE("exactly zero net return is not a positive outcome") {
    const auto r = label_of(rec("TIME_STOP", 0.0));
    REQUIRE(r.usable);
    REQUIRE(r.label == 0);
}

TEST_CASE("outcomes that never took a position are excluded") {
    // These measure the plan's entry geometry, which belongs to AltStrategy.
    // Scoring them as losses would train the filter to reject good setups whose
    // limit simply did not fill.
    for (const char* outcome : {"ENTRY_NOT_REACHED", "ENTRY_EXPIRED", "CANCELLED"}) {
        INFO(outcome);
        const auto r = label_of(rec(outcome, 0.0));
        REQUIRE_FALSE(r.usable);
        REQUIRE(r.rejection.find("took_no_position") != std::string::npos);
    }
}

TEST_CASE("outcomes AltStrategy declined to determine are excluded") {
    for (const char* outcome : {"AMBIGUOUS", "DATA_INCOMPLETE"}) {
        INFO(outcome);
        REQUIRE_FALSE(label_of(rec(outcome, 0.003)).usable);
    }
}

TEST_CASE("the upstream unusable flag is honoured even for a labelable outcome") {
    const auto r = label_of(rec("TARGET_FIRST", 0.004, /*usable=*/false));
    REQUIRE_FALSE(r.usable);
    REQUIRE(r.rejection == "upstream_binary_label_unusable");
}

TEST_CASE("a missing or non-finite net return is never guessed") {
    OutcomeRecord missing = rec("STOP_FIRST", 0.0);
    missing.net_return_present = false;
    REQUIRE_FALSE(label_of(missing).usable);

    OutcomeRecord nan_return = rec("STOP_FIRST", std::nan(""));
    REQUIRE_FALSE(label_of(nan_return).usable);
}

TEST_CASE("the SQL list and the predicate come from the same definition") {
    const std::string sql = labelable_outcomes_sql_list();
    REQUIRE(sql.front() == '(');
    REQUIRE(sql.back() == ')');
    for (const auto& o : kLabelableOutcomes) {
        INFO(o);
        REQUIRE(outcome_is_labelable(o));
        REQUIRE(sql.find("'" + std::string(o) + "'") != std::string::npos);
    }
    REQUIRE_FALSE(outcome_is_labelable("AMBIGUOUS"));
    REQUIRE(sql.find("AMBIGUOUS") == std::string::npos);
}

TEST_CASE("the published label spec describes the rule the code applies") {
    const std::string spec = label_spec_json();
    REQUIRE(spec.find("\"version\":\"sq-label:1\"") != std::string::npos);
    REQUIRE(spec.find("net_return_after_costs > 0") != std::string::npos);
    for (const auto& o : kLabelableOutcomes) {
        REQUIRE(spec.find(std::string(o)) != std::string::npos);
    }
    REQUIRE(std::string(kLabelSpecVersion) == "sq-label:1");
}
