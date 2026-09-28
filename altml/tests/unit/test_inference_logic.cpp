#include "altml/components/inference_logic.hpp"
#include "altml/core/codec.hpp"

#include <catch2/catch_test_macros.hpp>

#include <deque>

namespace {

altml::v1::FeatureVectorV1 valid_vector(int64_t now_ms) {
    altml::v1::FeatureVectorV1 fv;
    fv.set_timeframe(altml::v1::TF_1M);
    fv.set_bar_open_time_ms((now_ms / 60'000 - 1) * 60'000);
    fv.set_bar_revision(2);
    fv.set_feature_set_name(altml::analytics::kCoreFeatureSetName);
    fv.set_feature_set_version(altml::analytics::kCoreFeatureSetVersion);
    fv.set_schema_hash("schema");
    fv.set_engine_version(altml::analytics::kFeatureEngineVersion);
    fv.set_status(altml::v1::FEATURE_STATUS_VALID);
    fv.set_computed_at_ms(now_ms - 1'000);
    for (int i = 0; i < 9; ++i) fv.add_values(static_cast<float>(i));
    fv.set_validity_bitmap(std::string(2, static_cast<char>(0xff)));
    return fv;
}

} // namespace

TEST_CASE("inference freshness is anchored to source bar and durable computation") {
    constexpr int64_t now = 1'800'000;
    auto fv = valid_vector(now);
    CHECK(altml::inference::feature_rejection_reason(fv, now, 90'000, "schema", 9).empty());

    fv.set_bar_open_time_ms(now - 10 * 60'000);
    fv.set_computed_at_ms(now);
    CHECK(altml::inference::feature_rejection_reason(fv, now, 90'000, "schema", 9) ==
          "stale_or_future_bar");
}

TEST_CASE("inference rejects wrong contracts and bitmap shape") {
    constexpr int64_t now = 1'800'000;
    auto fv = valid_vector(now);
    fv.set_engine_version("old-engine");
    CHECK(altml::inference::feature_rejection_reason(fv, now, 90'000, "schema", 9) ==
          "feature_contract_mismatch");

    fv = valid_vector(now);
    fv.set_validity_bitmap(std::string(1, static_cast<char>(0xff)));
    CHECK(altml::inference::feature_rejection_reason(fv, now, 90'000, "schema", 9) ==
          "invalid_validity_bitmap");
}

// ------------------------------------------- reference_lite decoding

// AltTech publishes two payload shapes on the venue feature namespace:
// FeatureVector (talib-core) and VenueFeatureVector (reference_lite and the
// other per-venue sets). The second was dropped as "unsupported payload" --
// thousands a minute, terminated and gone -- because the decoder only knew the
// first. These call the production decoder, not a copy of it.

TEST_CASE("a reference_lite venue vector decodes with its venue and grid intact") {
    alttech::v1::Envelope in;
    in.set_schema_version(altml::messaging::kSchemaVersion);
    in.set_exchange(alttech::v1::EXCHANGE_BYBIT);
    in.set_instrument_id(42);
    in.set_exchange_symbol("BTCUSDT");
    auto* v = in.mutable_venue_feature_vector();
    v->set_scope(alttech::v1::ANALYTICS_SCOPE_VENUE);
    v->set_exchange(alttech::v1::EXCHANGE_BYBIT);
    v->set_timeframe(alttech::v1::TF_1M);
    v->set_bar_open_time_ms(1787000000000LL);
    v->set_bar_revision(3);
    v->set_profile("reference_lite");
    v->set_feature_set_name("reference_lite");
    v->set_feature_set_version(1);
    v->set_schema_hash("deadbeef");
    v->set_engine_version("alttech-reference-1");
    for (int i = 0; i < 31; ++i) v->add_values(static_cast<float>(i) * 1.5F);
    v->set_validity_bitmap(std::string(4, '\xff'));
    v->set_status(alttech::v1::ANALYTICS_VECTOR_STATUS_VALID);
    v->set_computed_at_ms(1787000060000LL);

    altml::v1::Envelope out;
    REQUIRE(altml::inference::repack_feature_envelope(in, &out));

    CHECK(static_cast<int>(out.exchange()) == static_cast<int>(altml::v1::EXCHANGE_BYBIT));
    const auto& fv = out.feature_vector();
    CHECK(fv.timeframe() == altml::v1::TF_1M);
    CHECK(fv.bar_open_time_ms() == 1787000000000LL);
    CHECK(fv.bar_revision() == 3);
    CHECK(fv.feature_set_name() == "reference_lite");
    CHECK(fv.feature_set_version() == 1);
    CHECK(fv.schema_hash() == "deadbeef");
    CHECK(fv.engine_version() == "alttech-reference-1");
    CHECK(fv.values_size() == 31);
    CHECK(fv.values(0) == 0.0F);
    CHECK(fv.values(30) == 45.0F);
    CHECK(fv.computed_at_ms() == 1787000060000LL);
    CHECK(fv.status() == altml::v1::FEATURE_STATUS_VALID);
}

TEST_CASE("a cross-venue consensus vector decodes, keyed by canonical pair") {
    // AltTech has published cross_venue_core on alttech.feature.cross_venue.>
    // since the module existed, while AltML's consumer filtered
    // alttech.feature.venue.> and its decoder knew two payload shapes. The
    // consensus view -- where the execution venue sits against all four venues,
    // who leads, how far Bitget lags -- was therefore never archived and could
    // not reach a model. Nothing failed: the messages were simply never
    // delivered, so no rejection appeared in any log.
    alttech::v1::Envelope in;
    in.set_schema_version(altml::messaging::kSchemaVersion);
    // A consensus belongs to no exchange; that is the producer's own encoding.
    in.set_exchange(alttech::v1::EXCHANGE_UNSPECIFIED);
    in.set_instrument_id(42);  // canonical pair id, the archive's instrument key
    in.set_canonical_pair_id(42);
    in.set_exchange_symbol("BTCUSDT");
    auto* c = in.mutable_cross_venue_feature_vector();
    c->set_scope(alttech::v1::ANALYTICS_SCOPE_CROSS_VENUE);
    c->set_canonical_pair_id(42);
    c->set_timeframe(alttech::v1::TF_1M);
    c->set_bar_open_time_ms(1787000000000LL);
    c->set_feature_set_name("cross_venue_core");
    c->set_feature_set_version(2);
    c->set_schema_hash("cafebabe");
    c->set_method_version("consensus-2");
    for (int i = 0; i < 37; ++i) c->add_values(static_cast<float>(i));
    c->set_validity_bitmap(std::string(5, '\xff'));
    // The producer never calls set_status on this shape; validity is carried by
    // quality_status. Left UNSPECIFIED here on purpose -- that is what arrives.
    c->set_quality_status(alttech::v1::CROSS_VENUE_QUALITY_STRICT_VALID);
    c->set_analytics_revision(7);
    c->set_computed_at_ms(1787000060000LL);

    altml::v1::Envelope out;
    REQUIRE(altml::inference::repack_feature_envelope(in, &out));

    CHECK(static_cast<int>(out.exchange()) == 0);  // stays unattributed
    CHECK(out.instrument_id() == 42);
    const auto& fv = out.feature_vector();
    CHECK(fv.feature_set_name() == "cross_venue_core");
    CHECK(fv.feature_set_version() == 2);
    CHECK(fv.values_size() == 37);
    CHECK(fv.values(36) == 36.0F);
    CHECK(fv.status() == altml::v1::FEATURE_STATUS_VALID);
    // The producer's names for the same two facts, carried across rather than
    // defaulted: a revision pinned to 0 would collide two computations of one
    // bar on the archive's primary key.
    CHECK(fv.bar_revision() == 7);
    CHECK(fv.engine_version() == "consensus-2");
}

TEST_CASE("consensus validity comes from quality_status, degraded stays degraded") {
    auto build = [](alttech::v1::CrossVenueQualityStatus q) {
        alttech::v1::Envelope in;
        in.set_schema_version(altml::messaging::kSchemaVersion);
        in.set_exchange(alttech::v1::EXCHANGE_UNSPECIFIED);
        in.set_instrument_id(7);
        auto* c = in.mutable_cross_venue_feature_vector();
        c->set_timeframe(alttech::v1::TF_1M);
        c->set_feature_set_name("cross_venue_core");
        c->set_feature_set_version(2);
        c->add_values(1.0F);
        c->set_quality_status(q);  // status left UNSPECIFIED, as the producer sends it
        return in;
    };
    altml::v1::Envelope out;

    REQUIRE(altml::inference::repack_feature_envelope(
        build(alttech::v1::CROSS_VENUE_QUALITY_STRICT_VALID), &out));
    CHECK(out.feature_vector().status() == altml::v1::FEATURE_STATUS_VALID);

    // 3-of-4 is a real consensus over fewer venues: not whole, not worthless.
    // Promoting it to VALID would archive a partial observation as a complete
    // one; demoting it to INVALID would throw away what it does say.
    REQUIRE(altml::inference::repack_feature_envelope(
        build(alttech::v1::CROSS_VENUE_QUALITY_DEGRADED_3_OF_4), &out));
    CHECK(out.feature_vector().status() == altml::v1::FEATURE_STATUS_INCOMPLETE_INPUT);

    REQUIRE(altml::inference::repack_feature_envelope(
        build(alttech::v1::CROSS_VENUE_QUALITY_INVALID), &out));
    CHECK(out.feature_vector().status() == altml::v1::FEATURE_STATUS_INVALID);

    // And an explicitly set status still wins, so this keeps working if the
    // producer starts filling the field.
    auto in = build(alttech::v1::CROSS_VENUE_QUALITY_INVALID);
    in.mutable_cross_venue_feature_vector()->set_status(
        alttech::v1::ANALYTICS_VECTOR_STATUS_WARMING);
    REQUIRE(altml::inference::repack_feature_envelope(in, &out));
    CHECK(out.feature_vector().status() == altml::v1::FEATURE_STATUS_WARMING);
}

TEST_CASE("the 1m microstructure window decodes; sub-minute windows are refused") {
    auto build = [](const char* window) {
        alttech::v1::Envelope in;
        in.set_schema_version(altml::messaging::kSchemaVersion);
        in.set_exchange(alttech::v1::EXCHANGE_BITGET);
        in.set_instrument_id(11);
        auto* m = in.mutable_microstructure_vector();
        m->set_window_label(window);
        m->set_window_start_ms(1787000040000LL);
        m->set_feature_set_name("bitget_micro");
        m->set_feature_set_version(1);
        m->set_schema_hash("micro-hash");
        m->set_engine_version("alttech-micro-1");
        for (int i = 0; i < 26; ++i) m->add_values(static_cast<float>(i) / 2.0F);
        m->set_validity_bitmap(std::string(4, '\xff'));
        m->set_status(alttech::v1::MICROSTRUCTURE_STATUS_VALID);
        m->set_computed_at_ms(1787000100000LL);
        return in;
    };
    altml::v1::Envelope out;

    // Tag 31 was RESERVED in the vendored envelope, so this payload was
    // "unsupported" and 1.28M messages -- the largest stream AltTech produces --
    // reached nothing.
    REQUIRE(altml::inference::repack_feature_envelope(build("1m"), &out));
    const auto& fv = out.feature_vector();
    CHECK(fv.timeframe() == altml::v1::TF_1M);
    CHECK(fv.bar_open_time_ms() == 1787000040000LL);
    CHECK(fv.feature_set_name() == "bitget_micro");
    CHECK(fv.values_size() == 26);
    CHECK(fv.status() == altml::v1::FEATURE_STATUS_VALID);
    // A tumbling window has no revision; 0 is the fact, not a placeholder.
    CHECK(fv.bar_revision() == 0);

    // 15s and 30s windows have no bar to belong to. Refused rather than rounded
    // into a minute they only partly cover -- which would archive a third of a
    // minute's book activity as if it were the whole minute's.
    CHECK_FALSE(altml::inference::repack_feature_envelope(build("15s"), &out));
    CHECK_FALSE(altml::inference::repack_feature_envelope(build("30s"), &out));

    // WARMING stays WARMING, NOT_AVAILABLE does not become a quiet zero.
    auto warming = build("1m");
    warming.mutable_microstructure_vector()->set_status(
        alttech::v1::MICROSTRUCTURE_STATUS_WARMING);
    REQUIRE(altml::inference::repack_feature_envelope(warming, &out));
    CHECK(out.feature_vector().status() == altml::v1::FEATURE_STATUS_WARMING);

    auto unavailable = build("1m");
    unavailable.mutable_microstructure_vector()->set_status(
        alttech::v1::MICROSTRUCTURE_STATUS_NOT_AVAILABLE);
    REQUIRE(altml::inference::repack_feature_envelope(unavailable, &out));
    CHECK(out.feature_vector().status() == altml::v1::FEATURE_STATUS_INVALID);
}

TEST_CASE("estimates this service does not produce are absent, not zero") {
    // A binary directional model has no view on magnitude, on which barrier is
    // touched first, or on holding time. Published as plain proto3 doubles and
    // never set, all five arrived as 0.0 and read exactly like a genuine
    // estimate of zero -- and AltStrategy's wire_codec reads all five.
    altml::v1::PredictionV1 p;
    p.set_probability_up(0.61);
    CHECK_FALSE(p.has_expected_return());
    CHECK_FALSE(p.has_probability_target_before_stop());
    CHECK_FALSE(p.has_expected_mfe());
    CHECK_FALSE(p.has_expected_mae());
    CHECK_FALSE(p.has_expected_holding_time_min());
    // Absent and zero are now distinguishable, which is the whole point.
    p.set_expected_return(0.0);
    CHECK(p.has_expected_return());
    CHECK(p.expected_return() == 0.0);
}

TEST_CASE("an unattributed exchange is refused for every shape but the consensus") {
    // EXCHANGE_UNSPECIFIED is meaningful for a cross-venue vector and missing
    // for everything else. Admitting it generally would file one venue's
    // observation under another.
    alttech::v1::Envelope in;
    in.set_schema_version(altml::messaging::kSchemaVersion);
    in.set_exchange(alttech::v1::EXCHANGE_UNSPECIFIED);
    auto* v = in.mutable_venue_feature_vector();
    v->set_timeframe(alttech::v1::TF_1M);
    v->set_feature_set_name("reference_lite");
    v->add_values(1.0F);
    altml::v1::Envelope out;
    CHECK_FALSE(altml::inference::repack_feature_envelope(in, &out));

    // And a consensus payload that claims a venue is equally refused.
    alttech::v1::Envelope wrong;
    wrong.set_schema_version(altml::messaging::kSchemaVersion);
    wrong.set_exchange(alttech::v1::EXCHANGE_BITGET);
    wrong.mutable_cross_venue_feature_vector()->set_feature_set_name("cross_venue_core");
    CHECK_FALSE(altml::inference::repack_feature_envelope(wrong, &out));
}

TEST_CASE("the global macro vector decodes with no venue and no instrument") {
    // AltTech's macro-core is one FeatureVector per minute for the whole
    // system: exchange unspecified and instrument 0, because the offshore
    // renminbi, the Treasury curve and VIX belong to no venue and no pair. The
    // decoder must let that shape through on the same path as the primary
    // venue's own vector, so the live aligned cache and the archive see it
    // exactly as the trainer's pinned join does. An unattributed exchange on a
    // vector that DOES claim an instrument stays a refusal: that is still a
    // venue's observation with its venue missing.
    alttech::v1::Envelope in;
    in.set_schema_version(altml::messaging::kSchemaVersion);
    in.set_exchange(alttech::v1::EXCHANGE_UNSPECIFIED);
    in.set_instrument_id(0);
    in.set_canonical_pair_id(0);
    in.set_exchange_symbol("GLOBAL");
    auto* fv = in.mutable_feature_vector();
    fv->set_timeframe(alttech::v1::TF_1M);
    fv->set_bar_open_time_ms(1787000000000LL);
    fv->set_feature_set_name("macro-core");
    fv->set_feature_set_version(1);
    fv->set_schema_hash("58bc7b7239b3");
    fv->set_engine_version("mc-1.2.0");
    for (int i = 0; i < 40; ++i) fv->add_values(static_cast<float>(i));
    fv->set_validity_bitmap(std::string(5, '\x0f'));
    fv->set_status(alttech::v1::FEATURE_STATUS_VALID);

    altml::v1::Envelope out;
    REQUIRE(altml::inference::repack_feature_envelope(in, &out));
    CHECK(static_cast<int>(out.exchange()) == 0);
    CHECK(out.instrument_id() == 0);
    CHECK(out.feature_vector().feature_set_name() == "macro-core");
    CHECK(out.feature_vector().values_size() == 40);
    CHECK(out.feature_vector().status() == altml::v1::FEATURE_STATUS_VALID);

    alttech::v1::Envelope claimed;
    claimed.CopyFrom(in);
    claimed.set_instrument_id(42);
    CHECK_FALSE(altml::inference::repack_feature_envelope(claimed, &out));
}

TEST_CASE("a warming venue vector stays warming") {
    alttech::v1::Envelope in;
    in.set_schema_version(altml::messaging::kSchemaVersion);
    in.set_exchange(alttech::v1::EXCHANGE_OKX);
    auto* v = in.mutable_venue_feature_vector();
    v->set_timeframe(alttech::v1::TF_1D);
    v->set_bar_open_time_ms(1787000000000LL);
    v->set_status(alttech::v1::ANALYTICS_VECTOR_STATUS_WARMING);
    altml::v1::Envelope out;
    REQUIRE(altml::inference::repack_feature_envelope(in, &out));
    // Turning WARMING into VALID would invent a readiness the producer never
    // claimed; the archive gate downstream refuses anything not VALID.
    CHECK(out.feature_vector().status() == altml::v1::FEATURE_STATUS_WARMING);
    CHECK(out.feature_vector().timeframe() == altml::v1::TF_1D);
}

TEST_CASE("an envelope with no feature payload at all is refused") {
    alttech::v1::Envelope in;
    in.set_schema_version(altml::messaging::kSchemaVersion);
    in.set_exchange(alttech::v1::EXCHANGE_BITGET);
    altml::v1::Envelope out;
    CHECK_FALSE(altml::inference::repack_feature_envelope(in, &out));
}

TEST_CASE("aligned history keeps bar order whatever order the bars arrive in",
          "[inference][aligned][regression]") {
    // AltTech backfills cross-venue minutes behind the newest one, so an older
    // bar can arrive after a newer. An arrival-ordered history put it at the
    // back, where latest_aligned() looks first, and a burst of eight of them
    // pushed the genuinely newest bar out.
    struct Obs {
        int64_t bar_open_ms = 0;
        int revision = 0;
    };
    constexpr size_t kCap = 8;
    constexpr int64_t kMin = 60'000;
    std::deque<Obs> hist;
    for (int64_t m = 100; m < 104; ++m) altml::inference::record_in_bar_order(hist, Obs{m * kMin}, kCap);

    SECTION("a backfilled older bar lands in place, the newest stays at the back") {
        altml::inference::record_in_bar_order(hist, Obs{98 * kMin}, kCap);
        REQUIRE(hist.size() == 5);
        CHECK(hist.front().bar_open_ms == 98 * kMin);
        CHECK(hist.back().bar_open_ms == 103 * kMin);
        for (size_t i = 1; i < hist.size(); ++i) CHECK(hist[i - 1].bar_open_ms < hist[i].bar_open_ms);
    }
    SECTION("a burst of backfilled bars never evicts the newest") {
        for (int64_t m = 99; m >= 60; --m) altml::inference::record_in_bar_order(hist, Obs{m * kMin}, kCap);
        REQUIRE(hist.size() == kCap);
        CHECK(hist.back().bar_open_ms == 103 * kMin);
        CHECK(hist.front().bar_open_ms == 96 * kMin);   // the eight newest bars
    }
    SECTION("a later revision of a held bar replaces it in place") {
        altml::inference::record_in_bar_order(hist, Obs{101 * kMin, 2}, kCap);
        REQUIRE(hist.size() == 4);
        CHECK(hist[1].bar_open_ms == 101 * kMin);
        CHECK(hist[1].revision == 2);
    }
    SECTION("a new bar still advances the window") {
        for (int64_t m = 104; m < 110; ++m) altml::inference::record_in_bar_order(hist, Obs{m * kMin}, kCap);
        REQUIRE(hist.size() == kCap);
        CHECK(hist.front().bar_open_ms == 102 * kMin);
        CHECK(hist.back().bar_open_ms == 109 * kMin);
    }
}
