#include "altmacro/components/publisher.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using altmacro::NatsConfig;
using altmacro::components::Publisher;
using altmacro::data::SourceBinding;
using altmacro::domain::PriceBasis;

TEST_CASE("dots in an instrument code become underscores", "[nats][subject]") {
    // A dot is NATS's token separator. Publishing to altmacro.bar.FX.USDCNH.bid
    // would split the instrument into two tokens and silently widen every
    // wildcard subscription downstream.
    CHECK(Publisher::subject_token("FX.USDCNH") == "FX_USDCNH");
    CHECK(Publisher::subject_token("RATE.US.10Y.REAL") == "RATE_US_10Y_REAL");
}

TEST_CASE("subjects follow the stack's existing shape", "[nats][subject]") {
    NatsConfig cfg;
    Publisher p(cfg, nullptr);
    CHECK(p.bar_subject("FX.USDCNH", PriceBasis::bid) == "altmacro.bar.FX_USDCNH.bid");
    CHECK(p.bar_subject("METAL.XAUUSD", PriceBasis::ask) == "altmacro.bar.METAL_XAUUSD.ask");
    CHECK(p.reference_subject("REF.USDCNY.PBOC") == "altmacro.reference.REF_USDCNY_PBOC");
}

TEST_CASE("bid and ask publish to different subjects", "[nats][subject]") {
    NatsConfig cfg;
    Publisher p(cfg, nullptr);
    CHECK(p.bar_subject("FX.USDCNH", PriceBasis::bid) !=
          p.bar_subject("FX.USDCNH", PriceBasis::ask));
}

TEST_CASE("message ids are deterministic and side-specific", "[nats][dedup]") {
    // A republish after a restart must deduplicate at the server rather than
    // appending a second copy of the same observation.
    SourceBinding b;
    b.instrument_code = "FX.USDCNH";
    b.provider_code = "dukascopy";
    const auto a1 = Publisher::message_id(b, "1m", 1788825600000LL, PriceBasis::bid);
    const auto a2 = Publisher::message_id(b, "1m", 1788825600000LL, PriceBasis::bid);
    const auto ask = Publisher::message_id(b, "1m", 1788825600000LL, PriceBasis::ask);
    CHECK(a1 == a2);
    CHECK(a1 != ask);
}

TEST_CASE("the same instant from two providers is two messages", "[nats][dedup]") {
    // Provenance is part of identity: the same number from a different source
    // is a different observation, not a duplicate to be collapsed.
    SourceBinding a; a.instrument_code = "REF.USDCNY.FED"; a.provider_code = "fred";
    SourceBinding b; b.instrument_code = "REF.USDCNY.FED"; b.provider_code = "frankfurter";
    CHECK(Publisher::message_id(a, "daily", 1000, PriceBasis::fixing) !=
          Publisher::message_id(b, "daily", 1000, PriceBasis::fixing));
}

TEST_CASE("semantic revisions publish while transport retries deduplicate", "[nats][dedup]") {
    nlohmann::json observation = {{"kind", "reference"}, {"instrument", "RATE.US.10Y"},
        {"event_time_ms", 1000}, {"provider", "fred"}, {"value", 4.1},
        {"quality", "VALID"}, {"publication_time", nullptr}, {"ingest_time", "first"}};
    const auto original = Publisher::revision_message_id(observation.dump());
    observation["ingest_time"] = "retry";
    CHECK(Publisher::revision_message_id(observation.dump()) == original);
    for (const auto& field : {"value", "quality", "publication_time", "provider", "instrument"}) {
        auto corrected = observation;
        corrected[field] = "correction";
        CHECK(Publisher::revision_message_id(corrected.dump()) != original);
    }
    auto bar = observation; bar["kind"] = "bar"; bar["close"] = 1.2;
    const auto bar_id = Publisher::revision_message_id(bar.dump());
    bar["close"] = 1.3;
    CHECK(Publisher::revision_message_id(bar.dump()) != bar_id);
    CHECK(bar_id != original);
}
