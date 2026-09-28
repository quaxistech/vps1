// The decision policy: how a calibrated score becomes ACCEPT/REJECT, and what
// the service does before it has a model. Cold start is the case that runs on
// day one in production, so it is tested exhaustively rather than assumed.

#include "altsignalquality/domain/policy.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <set>

using namespace altsignalquality::domain;
using Catch::Matchers::WithinAbs;

namespace {

struct Sample {
    std::vector<int> y;
    std::vector<double> score;
    std::vector<double> net;
};

// A slice where score genuinely ranks outcome: everything at or above 0.5 wins.
Sample separable(int n) {
    Sample s;
    for (int i = 0; i < n; ++i) {
        const bool good = i % 2 == 0;
        s.score.push_back(good ? 0.8 : 0.2);
        s.y.push_back(good ? 1 : 0);
        s.net.push_back(good ? 0.02 : -0.02);
    }
    return s;
}

} // namespace

TEST_CASE("the threshold is chosen where return per candidate is highest") {
    const auto s = separable(200);
    ThresholdSearchConfig cfg;
    cfg.min_accepted = 10;
    cfg.min_acceptance_rate = 0.05;
    const auto choice = choose_threshold(s.y, s.score, s.net, cfg);
    REQUIRE(choice.found);
    // Any cut between 0.2 and 0.8 keeps exactly the winners; the search prefers
    // the lowest such cut, because the one that trades more has more evidence.
    REQUIRE(choice.threshold > 0.2);
    REQUIRE(choice.threshold <= 0.8);
    REQUIRE_THAT(choice.evaluation.accepted.win_rate, WithinAbs(1.0, 1e-12));
    REQUIRE_THAT(choice.objective, WithinAbs(0.01, 1e-9));
}

TEST_CASE("a threshold that would trade too rarely is refused") {
    const auto s = separable(200);
    ThresholdSearchConfig cfg;
    cfg.min_accepted = 10;
    cfg.min_acceptance_rate = 0.90;   // no cut can accept 90% and stay selective
    const auto choice = choose_threshold(s.y, s.score, s.net, cfg);
    // Accepting everything is still a valid 100% cut, so the search finds one
    // only because it clears the floor; tighten the count instead.
    ThresholdSearchConfig strict = cfg;
    strict.min_accepted = 1000;
    const auto none = choose_threshold(s.y, s.score, s.net, strict);
    REQUIRE_FALSE(none.found);
    REQUIRE(none.rejection.find("min_accepted") != std::string::npos);
}

TEST_CASE("an empty or ragged calibration slice yields no threshold") {
    ThresholdSearchConfig cfg;
    REQUIRE_FALSE(choose_threshold({}, {}, {}, cfg).found);
    REQUIRE_FALSE(choose_threshold({1}, {0.5, 0.6}, {0.1}, cfg).found);
}

TEST_CASE("a learned score never decides: with or without a model the verdict is validity",
          "[policy][no-hidden-gate]") {
    // This stage used to accept only when its own model's score cleared a
    // fitted threshold -- a second scoring system after AltML's economics.
    DecisionConfig cfg;
    for (const double score : {0.01, 0.5, 0.99}) {
        INFO(score);
        const auto d = decide(cfg, /*upstream_recommended=*/true, /*scored=*/true, score,
                              /*threshold=*/0.70, false);
        REQUIRE(d.accepted);                       // valid: forwarded
        REQUIRE(d.basis == DecisionBasis::cold_start);
        REQUIRE(d.has_score);                      // the score is kept, as a diagnostic
        REQUIRE(d.reason_codes.back() == "DIAGNOSTIC_SCORE_NOT_USED");
    }
}

TEST_CASE("a valid signal is forwarded whatever AltStrategy's execution recommendation says",
          "[policy][vps1][regression]") {
    // PASS_THROUGH_RECOMMENDED + require_upstream_recommendation made
    // AltStrategy's economic verdict the gate: 8431 of 8431 signals rejected in
    // seven days, none reached VPS2. VPS1 takes no execution decision and the
    // flag is always false; validity is this service's only question.
    const DecisionConfig cfg;   // the defaults ARE the production policy
    REQUIRE(cfg.cold_start == ColdStartPolicy::pass_through_all);
    REQUIRE_FALSE(cfg.require_upstream_recommendation);
    for (const bool recommended : {false, true}) {
        const auto d = decide(cfg, recommended, /*scored=*/false, 0, 0, false);
        REQUIRE(d.accepted);
        REQUIRE(d.basis == DecisionBasis::cold_start);
        REQUIRE(d.reason_codes.back() == "COLD_START_PASS_THROUGH_ALL");
    }
}

TEST_CASE("the recommendation-based policy no longer parses", "[policy][vps1][regression]") {
    REQUIRE_FALSE(parse_cold_start_policy("PASS_THROUGH_RECOMMENDED").has_value());
}

TEST_CASE("cold start: REJECT_ALL and PASS_THROUGH_ALL are honest about themselves") {
    DecisionConfig reject;
    reject.cold_start = ColdStartPolicy::reject_all;
    const auto r = decide(reject, true, false, 0, 0, false);
    REQUIRE_FALSE(r.accepted);
    REQUIRE(r.reason_codes.back() == "COLD_START_REJECT_ALL");

    DecisionConfig all;
    all.cold_start = ColdStartPolicy::pass_through_all;
    const auto a = decide(all, false, false, 0, 0, false);
    REQUIRE(a.accepted);
    REQUIRE(a.reason_codes.back() == "COLD_START_PASS_THROUGH_ALL");
}

TEST_CASE("an unusable model is distinguished from having no model at all") {
    // These need different operator responses: one is "wait", the other is
    // "something is wrong with the artifact you promoted".
    DecisionConfig cfg;
    const auto d = decide(cfg, true, false, 0, 0, /*model_unusable=*/true);
    REQUIRE(d.basis == DecisionBasis::model_unusable);
    REQUIRE(d.reason_codes.front() == "MODEL_UNUSABLE");
    // It still falls back to the configured cold-start behaviour rather than
    // stalling the pipeline.
    REQUIRE(d.accepted);
}

TEST_CASE("a stale signal is rejected on its own terms, before the model") {
    // Reporting this as MODEL_UNUSABLE would send an operator after a model
    // that is perfectly fine, and reporting it as a score would imply the
    // signal was evaluated on a market that still exists.
    DecisionConfig cfg;
    const auto d = decide(cfg, /*upstream_recommended=*/true, /*scored=*/false, 0, 0,
                          /*model_unusable=*/false, /*stale=*/true);
    REQUIRE_FALSE(d.accepted);
    REQUIRE(d.basis == DecisionBasis::signal_stale);
    REQUIRE_FALSE(d.has_score);
    REQUIRE(d.reason_codes.front() == "SIGNAL_STALE");

    // Staleness wins over every other consideration, including a high score
    // and a permissive cold-start policy.
    DecisionConfig permissive;
    permissive.cold_start = ColdStartPolicy::pass_through_all;
    REQUIRE_FALSE(decide(permissive, true, true, 0.99, 0.5, false, true).accepted);
    REQUIRE_FALSE(decide(permissive, false, false, 0, 0, true, true).accepted);
}

TEST_CASE("cold start policy names round-trip through configuration") {
    for (const auto p : {ColdStartPolicy::pass_through_all, ColdStartPolicy::reject_all}) {
        const auto name = cold_start_policy_name(p);
        const auto parsed = parse_cold_start_policy(name);
        INFO(name);
        REQUIRE(parsed.has_value());
        REQUIRE(*parsed == p);
    }
    REQUIRE_FALSE(parse_cold_start_policy("MAYBE").has_value());
}

TEST_CASE("a persisted decision basis reads back as itself") {
    // Restart recovery rebuilds a published evaluation from the stored row; a
    // name that does not round-trip would republish a signal under the wrong
    // basis, quietly rewriting why the decision was taken.
    const std::vector<DecisionBasis> all = {
        DecisionBasis::model, DecisionBasis::cold_start, DecisionBasis::model_unusable,
        DecisionBasis::upstream_not_recommended, DecisionBasis::malformed_signal,
        DecisionBasis::score_below_floor, DecisionBasis::signal_stale};
    for (const auto b : all) {
        const auto parsed = parse_decision_basis(decision_basis_name(b));
        INFO(decision_basis_name(b));
        REQUIRE(parsed.has_value());
        REQUIRE(*parsed == b);
    }
    REQUIRE_FALSE(parse_decision_basis("SOMETHING_ELSE").has_value());
    REQUIRE_FALSE(parse_decision_basis("").has_value());
}

TEST_CASE("every decision basis has a distinct persisted name") {
    const std::vector<DecisionBasis> all = {
        DecisionBasis::model, DecisionBasis::cold_start, DecisionBasis::model_unusable,
        DecisionBasis::upstream_not_recommended, DecisionBasis::malformed_signal,
        DecisionBasis::score_below_floor, DecisionBasis::signal_stale};
    std::set<std::string_view> names;
    for (const auto b : all) {
        const auto name = decision_basis_name(b);
        INFO(name);
        REQUIRE(name != "?");
        REQUIRE(names.insert(name).second);
    }
}
