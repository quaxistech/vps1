// Every id this service creates is derived from its natural key, which is what
// makes a NATS redelivery or a restart reproduce the same row instead of a
// duplicate. If these stop holding, idempotency stops holding.

#include "altsignalquality/data/store.hpp"

#include <catch2/catch_test_macros.hpp>

#include <set>

using namespace altsignalquality::data;

TEST_CASE("ids are a pure function of the signal id") {
    const std::string signal = "sig_9f2c00a1";
    REQUIRE(make_evaluation_id(signal) == make_evaluation_id(signal));
    REQUIRE(make_final_signal_id(signal) == make_final_signal_id(signal));
}

TEST_CASE("different signals get different ids") {
    std::set<std::string> seen;
    for (int i = 0; i < 500; ++i) {
        const std::string signal = "sig_" + std::to_string(i);
        REQUIRE(seen.insert(make_evaluation_id(signal)).second);
        REQUIRE(seen.insert(make_final_signal_id(signal)).second);
    }
}

TEST_CASE("the evaluation and the final signal never share an id") {
    // They live in different namespaces and mean different things; colliding
    // them would let a JetStream de-duplication window swallow one of the two.
    const std::string signal = "sig_1";
    REQUIRE(make_evaluation_id(signal) != make_final_signal_id(signal));
    REQUIRE(make_evaluation_id(signal).rfind("sqev_", 0) == 0);
    REQUIRE(make_final_signal_id(signal).rfind("sqfin_", 0) == 0);
}

TEST_CASE("a training example id is keyed by the contracts it was built under") {
    const std::string signal = "sig_1";
    const auto a = make_example_id(signal, "sq-core:1", "sq-label:1");
    const auto b = make_example_id(signal, "sq-core:2", "sq-label:1");
    const auto c = make_example_id(signal, "sq-core:1", "sq-label:2");
    // A new feature schema or label spec produces NEW rows, so the history a
    // shipped model was trained on is never rewritten underneath it.
    REQUIRE(a != b);
    REQUIRE(a != c);
    REQUIRE(b != c);
    REQUIRE(a == make_example_id(signal, "sq-core:1", "sq-label:1"));
}

TEST_CASE("ids are fixed-width and safe as database keys") {
    const auto id = make_evaluation_id("sig with spaces / and 'quotes'");
    REQUIRE(id.size() == 5 + 32);
    REQUIRE(id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") == std::string::npos);
}
