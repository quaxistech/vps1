// Deterministic cross-venue engine tests.
#include "consensus.hpp"
#include "json_encode.hpp"
#include "alignment.hpp"
#include "normalization.hpp"
#include "lead_lag.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <string>
#include <vector>

using namespace alttech;
using namespace alttech::crossvenue;
using Catch::Matchers::WithinAbs;

namespace {

VenueSample mk(Venue v, double mid, VenueQuality q = VenueQuality::valid, double age = 100.0) {
    VenueSample s;
    s.venue = v;
    s.present = true;
    s.quality = q;
    s.data_age_ms = age;
    s.mid_price = mid;
    s.relative_spread = 0.0002;
    s.depth_usdt = 100000.0;
    return s;
}

VenueSample absent(Venue v) {
    VenueSample s;
    s.venue = v;
    s.present = false;
    s.quality = VenueQuality::not_available;
    return s;
}

} // namespace

TEST_CASE("weighted median picks the mass-crossing value, not the arithmetic mean") {
    std::vector<Contribution> c = {
        {Venue::bitget, 3.0, 100.0, true, 0, 0, 0, VenueQuality::valid, ""},
        {Venue::binance_usdm, 1.0, 101.0, true, 0, 0, 0, VenueQuality::valid, ""},
        {Venue::okx, 1.0, 200.0, true, 0, 0, 0, VenueQuality::valid, ""}, // outlier
    };
    const auto r = weighted_median(c);
    REQUIRE(r.available);
    // Total weight 5; half=2.5; cumulative crosses at value 100 (weight 3).
    CHECK_THAT(r.value, WithinAbs(100.0, 1e-9));
    // The arithmetic mean (120.6) would be dragged up by the outlier; median is not.
}

TEST_CASE("quality-weighted consensus down-weights a stale wide-spread venue") {
    std::vector<VenueSample> s;
    auto bg = mk(Venue::bitget, 100.0);
    auto bn = mk(Venue::binance_usdm, 100.1);
    auto by = mk(Venue::bybit, 99.9);
    auto ok = mk(Venue::okx, 130.0, VenueQuality::degraded, 5000.0); // stale + far
    ok.relative_spread = 0.02;
    ok.depth_usdt = 1000.0;
    s = {bg, bn, by, ok};
    WeightParams w;
    const auto r = consensus(s, w, ConsensusMethod::quality_weighted_median,
                             [](const VenueSample& x) { return x.mid_price; });
    REQUIRE(r.available);
    CHECK(r.value < 105.0); // the 130 outlier does not move the consensus
    // Every venue is recorded as a contribution for reproducibility.
    CHECK(r.contributions.size() == 4);
}

TEST_CASE("cross-venue quality classification degrades with missing references") {
    QualityParams qp;
    SECTION("all four valid -> STRICT_VALID") {
        std::vector<VenueSample> s = {mk(Venue::bitget, 100), mk(Venue::binance_usdm, 100),
                                      mk(Venue::bybit, 100), mk(Venue::okx, 100)};
        CHECK(classify_quality(s, qp, 100.0).status == CrossVenueQuality::strict_valid);
    }
    SECTION("bitget + two references -> DEGRADED_3_OF_4") {
        std::vector<VenueSample> s = {mk(Venue::bitget, 100), mk(Venue::binance_usdm, 100),
                                      mk(Venue::bybit, 100), absent(Venue::okx)};
        const auto d = classify_quality(s, qp, 100.0);
        CHECK(d.status == CrossVenueQuality::degraded_3_of_4);
        CHECK((d.missing_venue_mask & venue_mask_bit(Venue::okx)) != 0);
    }
    SECTION("bitget + one reference -> DEGRADED_2_OF_4") {
        std::vector<VenueSample> s = {mk(Venue::bitget, 100), mk(Venue::binance_usdm, 100),
                                      absent(Venue::bybit), absent(Venue::okx)};
        CHECK(classify_quality(s, qp, 100.0).status == CrossVenueQuality::degraded_2_of_4);
    }
    SECTION("stale Bitget -> INVALID") {
        std::vector<VenueSample> s = {mk(Venue::bitget, 100, VenueQuality::stale),
                                      mk(Venue::binance_usdm, 100), mk(Venue::bybit, 100),
                                      mk(Venue::okx, 100)};
        const auto d = classify_quality(s, qp, 100.0);
        CHECK(d.status == CrossVenueQuality::invalid);
        CHECK((d.stale_venue_mask & venue_mask_bit(Venue::bitget)) != 0);
    }
    SECTION("skew above threshold demotes STRICT to DEGRADED") {
        std::vector<VenueSample> s = {mk(Venue::bitget, 100), mk(Venue::binance_usdm, 100),
                                      mk(Venue::bybit, 100), mk(Venue::okx, 100)};
        const auto d = classify_quality(s, qp, qp.max_cross_venue_skew_ms + 1.0);
        CHECK(d.status != CrossVenueQuality::strict_valid);
    }
}

TEST_CASE("bar-window alignment status follows.1") {
    AlignmentParams ap;
    const std::vector<VenueSample> four = {mk(Venue::bitget, 100), mk(Venue::binance_usdm, 100),
                                           mk(Venue::bybit, 100), mk(Venue::okx, 100)};
    SECTION("4/4 on time -> STRICT_ALIGNED, ready") {
        const auto d = evaluate_bar_window(four, ap, 100.0, false);
        CHECK(d.status == AlignmentStatus::strict_aligned);
        CHECK(d.ready);
    }
    SECTION("3/4 before watermark -> WAITING, not ready") {
        std::vector<VenueSample> s = {mk(Venue::bitget, 100), mk(Venue::binance_usdm, 100),
                                      mk(Venue::bybit, 100), absent(Venue::okx)};
        const auto d = evaluate_bar_window(s, ap, 0.0, false);
        CHECK(d.status == AlignmentStatus::waiting);
        CHECK_FALSE(d.ready);
    }
    SECTION("3/4 after watermark -> DEGRADED_ALIGNED, ready") {
        std::vector<VenueSample> s = {mk(Venue::bitget, 100), mk(Venue::binance_usdm, 100),
                                      mk(Venue::bybit, 100), absent(Venue::okx)};
        const auto d = evaluate_bar_window(s, ap, ap.bar_watermark_timeout_ms + 1.0, false);
        CHECK(d.status == AlignmentStatus::degraded_aligned);
        CHECK(d.ready);
    }
    SECTION("stale Bitget after watermark -> INVALID") {
        std::vector<VenueSample> s = {absent(Venue::bitget), mk(Venue::binance_usdm, 100),
                                      mk(Venue::bybit, 100), mk(Venue::okx, 100)};
        const auto d = evaluate_bar_window(s, ap, ap.bar_watermark_timeout_ms + 1.0, false);
        CHECK(d.status == AlignmentStatus::invalid);
    }
    SECTION("late venue after emit -> LATE_REVISION") {
        auto s = four;
        s[3].late = true;
        const auto d = evaluate_bar_window(s, ap, 100.0, /*already_emitted=*/true);
        CHECK(d.status == AlignmentStatus::late_revision);
        CHECK(d.ready);
    }
}

TEST_CASE("funding normalization compares venues on the same time base") {
    // Binance/OKX quote different intervals; normalize both to 8h-equivalent.
    const auto f8 = normalize_funding(0.0001, 8.0);   // already 8h
    const auto f1 = normalize_funding(0.0001 / 8.0, 1.0); // 1h rate, same 8h total
    REQUIRE(f8);
    REQUIRE(f1);
    CHECK_THAT(f8->rate_8h_equivalent, WithinAbs(f1->rate_8h_equivalent, 1e-12));
    CHECK_THAT(f8->rate_per_hour, WithinAbs(0.0000125, 1e-12));
    // Unknown interval is never guessed.
    CHECK_FALSE(normalize_funding(0.0001, 0.0).has_value());
}

TEST_CASE("OI relative change and robust normalization") {
    CHECK_THAT(*oi_relative_change(110.0, 100.0), WithinAbs(0.10, 1e-12));
    CHECK_FALSE(oi_relative_change(100.0, 0.0).has_value());   // no previous OI
    const std::array<double, 5> w = {1, 2, 3, 4, 5};
    CHECK_THAT(mean(w), WithinAbs(3.0, 1e-12));
    CHECK(zscore(5.0, w).has_value());
    CHECK_THAT(percentile_rank(3.0, w), WithinAbs(0.6, 1e-12)); // 3 of 5 <= 3
}

TEST_CASE("lead-lag detects the true leader and corrects for clock skew") {
    // binance is the leader; bitget follows by exactly 2 samples.
    std::vector<double> lead = {0, 1, -1, 2, -2, 1, 0, 1, -1, 2, -2, 1};
    std::vector<double> follow(lead.size(), 0.0);
    for (size_t i = 2; i < lead.size(); ++i) follow[i] = lead[i - 2];

    std::array<double, 12> b{}, n{};
    for (size_t i = 0; i < lead.size(); ++i) { n[i] = lead[i]; b[i] = follow[i]; }

    std::vector<VenueSeries> series = {
        {Venue::binance_usdm, std::span<const double>(n), 0.0},
        {Venue::bitget, std::span<const double>(b), 0.0},
    };
    LeadLagParams p;
    p.max_lag = 4;
    p.sample_interval_ms = 1000.0;
    const auto r = compute_lead_lag(series, p);
    CHECK(r.dominant_leading_venue == Venue::binance_usdm);
    // Bitget follows binance by ~2 samples = ~2000 ms.
    CHECK(r.bitget_lag_to_binance_ms > 1000.0);

    // A false lead caused purely by clock skew is corrected out: Bitget appears
    // to lag Binance by exactly 2 samples, but that apparent lag is entirely due
    // to Binance's clock running 2s ahead. After correction the true lag is ~0.
    std::vector<VenueSeries> skewed = {
        {Venue::binance_usdm, std::span<const double>(n), 2000.0}, // 2s ahead clock
        {Venue::bitget, std::span<const double>(b), 0.0},          // b lags n by 2 samples
    };
    const auto r2 = compute_lead_lag(skewed, p);
    CHECK(std::abs(r2.bitget_lag_to_binance_ms) < 500.0); // clock-only lead removed
}

TEST_CASE("deviation and dispersion in basis points") {
    CHECK_THAT(deviation_bps(100.1, 100.0), WithinAbs(10.0, 1e-9)); // +0.1% = 10 bps
    std::vector<Contribution> c = {
        {Venue::bitget, 1.0, 100.0, true, 0, 0, 0, VenueQuality::valid, ""},
        {Venue::binance_usdm, 1.0, 100.2, true, 0, 0, 0, VenueQuality::valid, ""},
    };
    CHECK(dispersion_bps(c, 100.1) > 0.0);
}

// The quality reasons are stored in a JSONB column. SQL quoting produces
// single-quoted literals, which PostgreSQL rejects as JSON, so the encoding is
// explicit and tested rather than improvised.
TEST_CASE("quality reasons are encoded as JSON, not as SQL literals") {
    CHECK(json_string_array({}) == "[]");
    CHECK(json_string_array({"okx:stale"}) == R"(["okx:stale"])");
    CHECK(json_string_array({"okx:stale", "skew_exceeded"}) ==
          R"(["okx:stale","skew_exceeded"])");
    // Quotes, backslashes and control characters must not break the document.
    CHECK(json_string_array({"a\"b"}) == "[\"a\\\"b\"]");
    CHECK(json_string_array({"a\\b"}) == "[\"a\\\\b\"]");
    CHECK(json_string_array({"a\nb"}) == "[\"a\\nb\"]");
    CHECK(json_string_array({std::string(1, char(1))}) == "[\"\\u0001\"]");
}
