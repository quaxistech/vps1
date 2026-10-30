// The defaults in ProjectConfig are what a config file missing a key silently
// falls back to, so they are part of the shipped contract and are pinned here.
//
// Regression: they described the contract as it stood BEFORE the subscription
// was widened. `durable` still named `inference-features-v2` and
// `input_subject_filters` still held the single `alttech.feature.>` root, while
// the shipped /etc/altml/altml.yaml had moved to `inference-features-v4` and
// two filters. Dropping either key from the YAML would therefore have
// reattached the service to the narrow contract -- and because ensure_consumer
// refuses to mutate a durable whose contract has drifted, it would have bound
// to the superseded consumer instead of failing, so the execution venue's
// book/tape would simply have stopped arriving. That is the exact failure the
// widening was made to fix.

#include "altml/core/config.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using altml::config::ProjectConfig;

TEST_CASE("project defaults state the shipped subscription contract") {
    const ProjectConfig p;

    SECTION("the durable is the one the current filter set requires") {
        REQUIRE(p.durable == "inference-features-v4");
    }

    SECTION("both input roots are subscribed, not just the venue vectors") {
        REQUIRE(p.input_subject_filters.size() == 2);
        const auto has = [&](std::string_view s) {
            return std::find(p.input_subject_filters.begin(), p.input_subject_filters.end(), s) !=
                   p.input_subject_filters.end();
        };
        REQUIRE(has("alttech.feature.>"));
        // Window identities and configured capture keep fast sources separate.
        REQUIRE(has("alttech.microfeature.venue.>"));
    }
}
