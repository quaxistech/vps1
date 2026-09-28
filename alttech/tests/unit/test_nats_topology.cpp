#include "alttech/messaging/topology.hpp"

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

namespace topology = alttech::messaging::topology;

TEST_CASE("NATS topology defaults are AltTech-owned and vendor-neutral") {
    STATIC_REQUIRE(topology::kDefaultInputStream == "ALTSPOTTER_BARS");
    STATIC_REQUIRE(topology::kDefaultOutputStream == "ALTTECH_ANALYTICS");
    STATIC_REQUIRE(topology::kDefaultBarConsumer == "alttech-bars");
    // The durable filters to the primary venue: AltSpotter publishes all four
    // venues under bar.>, and taking delivery of bars nothing consumes would
    // only build an unacknowledged backlog.
    STATIC_REQUIRE(topology::kBarSubject == "bar.bitget.>");
    STATIC_REQUIRE(topology::kAnalyticsSubject == "alttech.>");
}

TEST_CASE("output subjects live under the owned alttech namespace") {
    using alttech::Timeframe;
    using alttech::Venue;
    CHECK(topology::venue_feature_subject(Venue::bitget, "BTCUSDT", Timeframe::m1) ==
          "alttech.feature.venue.bitget.BTCUSDT.1m.core.v1");
    CHECK(topology::venue_feature_subject(Venue::okx, "BTC-USDT-SWAP", Timeframe::m1) ==
          "alttech.feature.venue.okx.BTC-USDT-SWAP.1m.reference.v1");
    CHECK(topology::cross_venue_feature_subject("BTCUSDT", Timeframe::m1) ==
          "alttech.feature.cross_venue.BTCUSDT.1m.core.v1");
    CHECK(topology::cross_venue_regime_subject("BTCUSDT", Timeframe::m1) ==
          "alttech.regime.cross_venue.BTCUSDT.1m.v1");
    CHECK(topology::bitget_projected_structure_subject("BTCUSDT", Timeframe::m1) ==
          "alttech.structure.bitget_projected.BTCUSDT.1m.v1");
}

TEST_CASE("NATS resource names use the portable safe subset") {
    CHECK(topology::valid_resource_name("ALTSPOTTER_BARS"));
    CHECK(topology::valid_resource_name("alttech-bars"));
    CHECK_FALSE(topology::valid_resource_name(""));
    CHECK_FALSE(topology::valid_resource_name("ALTSPOTTER.BARS"));
    CHECK_FALSE(topology::valid_resource_name("bars.*"));
    CHECK_FALSE(topology::valid_resource_name("bars/consumer"));
    CHECK_FALSE(topology::valid_resource_name(std::string(256, 'a')));
}

TEST_CASE("an external stream only has to cover the subject AltTech reads") {
    using topology::subject_covers;
    // AltSpotter configures ALTSPOTTER_BARS as bar.> for its own needs; that
    // covers the primary-venue filter AltTech's durable uses.
    STATIC_REQUIRE(subject_covers("bar.>", "bar.bitget.>"));
    STATIC_REQUIRE(subject_covers("bar.>", "bar.bitget.BTCUSDT.1m.closed"));
    STATIC_REQUIRE(subject_covers("bar.bitget.>", "bar.bitget.>"));
    STATIC_REQUIRE(subject_covers("bar.*.BTCUSDT.>", "bar.bitget.BTCUSDT.1m.closed"));
    STATIC_REQUIRE(subject_covers(">", "bar.bitget.>"));

    // A narrower stream does not cover a broader filter.
    STATIC_REQUIRE_FALSE(subject_covers("bar.bitget.>", "bar.>"));
    STATIC_REQUIRE_FALSE(subject_covers("bar.okx.>", "bar.bitget.>"));
    STATIC_REQUIRE_FALSE(subject_covers("md.>", "bar.bitget.>"));
    STATIC_REQUIRE_FALSE(subject_covers("bar.bitget", "bar.bitget.BTCUSDT"));
    STATIC_REQUIRE_FALSE(subject_covers("bar.*", "bar.bitget.BTCUSDT"));
}

// --------------------------------------------------- venue-aware publication

// This test exists because the publication path once hardcoded the venue in two
// places at once: the subject was built with a literal Venue::bitget, and
// make_envelope() stamped EXCHANGE_BITGET into every envelope. AltTech computed
// all four venues and published them all as Bitget's -- wrong subject, wrong
// payload -- so a downstream consumer reading the venue off the envelope filed
// Binance's vector as Bitget's. Nothing failed; the numbers were simply wrong.
//
// It calls the production functions, not a copy of the string they build.
TEST_CASE("a feature subject names the venue it was computed for") {
    using alttech::Venue;
    using alttech::messaging::topology::venue_feature_subject;

    struct Case { Venue venue; const char* token; };
    for (const auto& c : {Case{Venue::bitget, "bitget"},
                          Case{Venue::binance_usdm, "binance_usdm"},
                          Case{Venue::okx, "okx"},
                          Case{Venue::bybit, "bybit"}}) {
        const std::string subject = venue_feature_subject(c.venue, "BTCUSDT", alttech::Timeframe::m1);
        INFO("subject = " << subject);
        CHECK(subject.find(std::string("alttech.feature.venue.") + c.token + ".") == 0);
    }

    // And the four must be distinct: a collapse here is the original defect.
    std::set<std::string> subjects;
    for (const Venue v : alttech::kAllVenues) {
        subjects.insert(venue_feature_subject(v, "BTCUSDT", alttech::Timeframe::m1));
    }
    CHECK(subjects.size() == alttech::kAllVenues.size());
}
