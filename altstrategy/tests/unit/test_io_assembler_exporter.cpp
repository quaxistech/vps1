// IO helper tests that need neither a database nor NATS: the AltTech ordered-
// vector decode and the snapshot exporter's columnar writer + manifest + SHA-256.

#include "altstrategy/engine_io/context_assembler.hpp"
#include "altstrategy/engine_io/snapshot_exporter.hpp"
#include "altstrategy/engine_io/context_json.hpp"
#include "engine_test_helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>

using namespace altstrategy::engine_io;
namespace fs = std::filesystem;

namespace {

// A stand-in producer set: five members, pinned by identity exactly as the real
// AltTech sets are.
PositionalFeatureSet test_set(size_t value_count = 5) {
    PositionalFeatureSet s;
    s.name = "talib-core";
    s.version = 1;
    s.schema_hash = "hash-A";
    s.value_count = value_count;
    s.positions = {{"px", 0}, {"ema_20", 1}, {"ema_50", 2}, {"atr_14", 3}, {"adx_14", 4}};
    return s;
}

ParsedAltTech bitget_message(int64_t pair, std::vector<float> values, int32_t timeframe = 15) {
    ParsedAltTech p;
    p.kind = AltTechKind::bitget_features;
    p.canonical_pair_id = pair;
    p.bar_open_time_ms = 1'000'000;
    p.revision = 3;
    p.timeframe_minutes = timeframe;
    p.feature_set_name = "talib-core";
    p.feature_set_version = 1;
    p.schema_hash = "hash-A";
    p.values = std::move(values);
    p.features.valid = true;
    return p;
}

} // namespace

TEST_CASE("decode_vector binds producer positions to engine names and drops NaN") {
    // Values chosen to be exactly representable in float so == holds after the
    // float→double widening.
    auto msg = bitget_message(1, {100.0f, 0.5f, 0.25f, std::nanf(""), 28.0f});
    const auto f = decode_vector(test_set(), msg);
    CHECK(f.valid);
    CHECK(f.revision == 3);
    CHECK(f.get("px") == 100.0);
    CHECK(f.get("ema_20") == 0.5);
    CHECK(f.get("adx_14") == 28.0);
    CHECK_FALSE(f.has("atr_14"));   // NaN dropped, never read as data
}

TEST_CASE("decode_vector refuses a vector from a different producer set") {
    // The real failure this guards: AltTech's talib-core is 995 members, and a
    // consumer holding a short hand-written list bound position 9 (a 20-bar
    // rolling high) to "atr_14" and position 22 (a bar index) to "spread_bps" —
    // values that look like data and are not.
    SECTION("length differs") {
        auto msg = bitget_message(1, std::vector<float>(995, 1.0f));
        const auto f = decode_vector(test_set(), msg);
        CHECK(f.schema_mismatch);
        CHECK_FALSE(f.valid);
        CHECK(f.producer_value_count == 995);
        CHECK(f.values.empty());
    }
    SECTION("schema hash differs") {
        auto msg = bitget_message(1, {1.0f, 2.0f, 3.0f, 4.0f, 5.0f});
        msg.schema_hash = "hash-B";        // producer re-ordered its members
        const auto f = decode_vector(test_set(), msg);
        CHECK(f.schema_mismatch);
        CHECK(f.values.empty());
    }
    SECTION("set name differs") {
        auto msg = bitget_message(1, {1.0f, 2.0f, 3.0f, 4.0f, 5.0f});
        msg.feature_set_name = "reference_lite";
        const auto f = decode_vector(test_set(), msg);
        CHECK(f.schema_mismatch);
        CHECK(f.values.empty());
    }
    SECTION("nothing configured") {
        auto msg = bitget_message(1, {1.0f, 2.0f, 3.0f, 4.0f, 5.0f});
        const auto f = decode_vector(PositionalFeatureSet{}, msg);
        CHECK(f.schema_mismatch);
        CHECK(f.values.empty());
    }
}

TEST_CASE("decode_vector drops slots the producer marked unusable") {
    auto msg = bitget_message(1, {10.0f, 20.0f, 30.0f, 40.0f, 50.0f});
    // Bit i in byte i/8 at offset i%8, least significant first: 0b00010101
    // marks positions 0, 2 and 4 usable, leaving 1 and 3 unusable.
    msg.validity_bitmap = std::string("\x15", 1);
    const auto f = decode_vector(test_set(), msg);
    CHECK(f.valid);
    CHECK(f.get("px") == 10.0);
    CHECK(f.get("ema_50") == 30.0);
    CHECK(f.get("adx_14") == 50.0);
    CHECK_FALSE(f.has("ema_20"));
    CHECK_FALSE(f.has("atr_14"));
    CHECK(f.values.size() == 3);
}

TEST_CASE("an unreadable vector never becomes a decision context") {
    AssemblerConfig cfg;
    cfg.bitget_features = test_set();     // expects 5 members
    ContextAssembler asm_(cfg);

    asm_.ingest_alttech(bitget_message(7, std::vector<float>(995, 1.0f)));

    ParsedAltTech reg;
    reg.kind = AltTechKind::global_regime;
    reg.canonical_pair_id = 7;
    reg.regime.regime = "TREND_UP";
    reg.regime.confidence = 0.7;
    asm_.ingest_alttech(reg);

    // Both components "arrived", but the feature vector could not be read, so
    // no proposal may be derived from it.
    CHECK_FALSE(asm_.try_build(7, 1'000'100).has_value());
}

TEST_CASE("only the decision timeframe enters the context") {
    AssemblerConfig cfg;
    cfg.bitget_features = test_set();
    cfg.decision_timeframe_minutes = 15;
    ContextAssembler asm_(cfg);

    // A daily vector on the same subject must not become the context: mixing
    // bar lengths would size a stop from whichever arrived last.
    asm_.ingest_alttech(bitget_message(9, {1.0f, 2.0f, 3.0f, 4.0f, 5.0f}, /*timeframe=*/1440));
    ParsedAltTech reg;
    reg.kind = AltTechKind::global_regime;
    reg.canonical_pair_id = 9;
    reg.regime.regime = "TREND_UP";
    asm_.ingest_alttech(reg);
    CHECK_FALSE(asm_.try_build(9, 1'000'100).has_value());

    // The configured timeframe is admitted.
    asm_.ingest_alttech(bitget_message(9, {100.0f, 2.0f, 3.0f, 4.0f, 5.0f}, /*timeframe=*/15));
    auto ctx = asm_.try_build(9, 1'000'100);
    REQUIRE(ctx.has_value());
    CHECK(ctx->bitget_features.get("px") == 100.0);
}

TEST_CASE("context assembler needs the core components before it emits a context") {
    AssemblerConfig cfg;
    cfg.bitget_features = test_set();
    ContextAssembler asm_(cfg);
    // Only Bitget features so far → not enough to build.
    asm_.ingest_alttech(bitget_message(42, {100.0f, 0.99f, 0.97f, 1.0f, 28.0f}));
    CHECK_FALSE(asm_.try_build(42, 1'000'100).has_value());

    // Add the global regime → now a context can be assembled.
    ParsedAltTech reg;
    reg.kind = AltTechKind::global_regime;
    reg.canonical_pair_id = 42;
    reg.regime.regime = "TREND_UP";
    reg.regime.confidence = 0.7;
    asm_.ingest_alttech(reg);
    auto ctx = asm_.try_build(42, 1'000'100);
    REQUIRE(ctx.has_value());
    CHECK(ctx->canonical_pair_id == 42);
    CHECK(ctx->global_regime.regime == "TREND_UP");
    CHECK(ctx->bitget_features.get("px") == 100.0);
    CHECK(ctx->context_revision > 0);
    // Idempotent: no new revision => no re-emit.
    CHECK_FALSE(asm_.try_build(42, 1'000'200).has_value());
}

TEST_CASE("volume surge is the log of the producer's ratio, floored") {
    AssemblerConfig cfg;
    cfg.bitget_features = test_set();
    cfg.microstructure_features.name = "bitget_micro";
    cfg.microstructure_features.version = 1;
    cfg.microstructure_features.schema_hash = "micro-hash";
    cfg.microstructure_features.value_count = 1;
    cfg.microstructure_features.positions = {{"turnover_acceleration", 0}};

    const auto surge_for = [&](float ratio) {
        ContextAssembler asm_(cfg);
        asm_.ingest_alttech(bitget_message(4, {1.0f, 2.0f, 3.0f, 4.0f, 5.0f}));
        ParsedAltTech micro;
        micro.kind = AltTechKind::bitget_microstructure;
        micro.window_label = "1m";
        micro.canonical_pair_id = 4;
        micro.bar_open_time_ms = 1'000'000;
        micro.feature_set_name = "bitget_micro";
        micro.feature_set_version = 1;
        micro.schema_hash = "micro-hash";
        micro.values = {ratio};
        micro.features.valid = true;
        asm_.ingest_alttech(micro);
        ParsedAltTech reg;
        reg.kind = AltTechKind::global_regime;
        reg.canonical_pair_id = 4;
        reg.regime.regime = "TREND_UP";
        asm_.ingest_alttech(reg);
        auto ctx = asm_.try_build(4, 1'000'100);
        REQUIRE(ctx.has_value());
        return ctx->microstructure.get("volume_surge", -999.0);
    };

    // Unchanged volume is the origin, not 1.0 — the thresholds are read in
    // sigma-like units, so "no change" has to sit at zero.
    CHECK(surge_for(1.0F) == Catch::Approx(0.0));
    CHECK(surge_for(2.718281828F) == Catch::Approx(1.0).margin(1e-4));
    // A collapsed denominator used to send this to 120000; it is now bounded.
    CHECK(surge_for(120000.0F) < 12.0);
    // Zero volume carries no magnitude information: floored, never -infinity.
    CHECK(surge_for(0.0F) == Catch::Approx(-5.0));
    CHECK(std::isfinite(surge_for(0.0F)));
}

TEST_CASE("only the pinned microstructure window enters the context") {
    // AltTech publishes the SAME 26 columns on 1s/5s/15s/30s/1m. The assembler
    // ordered readings by time and kept the newest, which is always a 1s window
    // -- 924k of them in the stream against 17k at 1m. Two things broke at once:
    //
    //   units  - trade_intensity, signed_volume and cvd_delta are per-window
    //            quantities, so the context carried per-second numbers while the
    //            thresholds read them as per-minute. Measured on VPS1, mean
    //            |cvd_delta| in built contexts was 407 against 28 301 in the 1m
    //            windows.
    //   book   - a 1s window usually has no book snapshot, so book_imbalance and
    //            book_microprice_dev reached 260 of 2187 contexts (12%), and
    //            every strategy that requires the resting book was starved.
    AssemblerConfig cfg;
    cfg.bitget_features = test_set();
    cfg.microstructure_window = "1m";
    cfg.microstructure_features.name = "bitget_micro";
    cfg.microstructure_features.version = 1;
    cfg.microstructure_features.schema_hash = "micro-hash";
    cfg.microstructure_features.value_count = 2;
    cfg.microstructure_features.positions = {{"cvd_delta", 0}, {"book_imbalance", 1}};

    const auto micro_msg = [](const char* window, int64_t when, float delta, float imbalance) {
        ParsedAltTech m;
        m.kind = AltTechKind::bitget_microstructure;
        m.canonical_pair_id = 4;
        m.window_label = window;
        m.bar_open_time_ms = when;
        m.feature_set_name = "bitget_micro";
        m.feature_set_version = 1;
        m.schema_hash = "micro-hash";
        m.values = {delta, imbalance};
        m.features.valid = true;
        return m;
    };

    ContextAssembler asm_(cfg);
    asm_.ingest_alttech(bitget_message(4, {1.0f, 2.0f, 3.0f, 4.0f, 5.0f}));
    ParsedAltTech reg;
    reg.kind = AltTechKind::global_regime;
    reg.canonical_pair_id = 4;
    reg.regime.regime = "TREND_UP";
    asm_.ingest_alttech(reg);

    // The 1m reading arrives first; a NEWER 1s reading follows and must not
    // displace it, however fresh it is.
    asm_.ingest_alttech(micro_msg("1m", 1'000'000, 28301.0F, 0.42F));
    asm_.ingest_alttech(micro_msg("1s", 1'000'900, 407.0F, 0.0F));

    auto ctx = asm_.try_build(4, 1'001'000);
    REQUIRE(ctx.has_value());
    CHECK(ctx->microstructure.get("cvd_delta", -1.0) == Catch::Approx(28301.0));
    CHECK(ctx->microstructure.has("book_imbalance"));
    CHECK(ctx->microstructure.get("book_imbalance", -1.0) == Catch::Approx(0.42));
}

TEST_CASE("a microstructure window that is not the pinned one is not a context input") {
    AssemblerConfig cfg;
    cfg.bitget_features = test_set();
    cfg.microstructure_window = "1m";
    cfg.microstructure_features.name = "bitget_micro";
    cfg.microstructure_features.version = 1;
    cfg.microstructure_features.schema_hash = "micro-hash";
    cfg.microstructure_features.value_count = 1;
    cfg.microstructure_features.positions = {{"cvd_delta", 0}};

    ContextAssembler asm_(cfg);
    asm_.ingest_alttech(bitget_message(4, {1.0f, 2.0f, 3.0f, 4.0f, 5.0f}));
    ParsedAltTech reg;
    reg.kind = AltTechKind::global_regime;
    reg.canonical_pair_id = 4;
    reg.regime.regime = "TREND_UP";
    asm_.ingest_alttech(reg);

    ParsedAltTech m;
    m.kind = AltTechKind::bitget_microstructure;
    m.canonical_pair_id = 4;
    m.window_label = "15s";
    m.bar_open_time_ms = 1'000'000;
    m.feature_set_name = "bitget_micro";
    m.feature_set_version = 1;
    m.schema_hash = "micro-hash";
    m.values = {8216.0F};
    m.features.valid = true;
    asm_.ingest_alttech(m);

    auto ctx = asm_.try_build(4, 1'000'100);
    REQUIRE(ctx.has_value());
    // Absent, not defaulted: a 15s reading is not a 1m reading scaled down, and
    // substituting one for the other is the unit error this pins down.
    CHECK_FALSE(ctx->microstructure.has("cvd_delta"));
}

TEST_CASE("mark and index are carried separately from the consensus mid") {
    // `index_price` was declared on DecisionContext and assigned by nothing, so
    // every strategy gated on `index_price > 0` -- the whole basis family --
    // could never fire. `mark_price` meanwhile held the consensus MID, which is
    // correct for last_price() and wrong for a basis: (mid - index) is a
    // different quantity from (mark - index) and looks just as plausible.
    AssemblerConfig cfg;
    cfg.bitget_features = test_set();
    cfg.cross_venue_features.name = "cross_venue_core";
    cfg.cross_venue_features.version = 2;
    cfg.cross_venue_features.value_count = 3;
    cfg.cross_venue_features.positions = {{"consensus_mid_price", 0},
                                          {"consensus_mark_price", 1},
                                          {"consensus_index_price", 2}};

    ContextAssembler asm_(cfg);
    asm_.ingest_alttech(bitget_message(4, {1.0f, 2.0f, 3.0f, 4.0f, 5.0f}));
    ParsedAltTech cv;
    cv.kind = AltTechKind::cross_venue_features;
    cv.canonical_pair_id = 4;
    cv.bar_open_time_ms = 1'000'000;
    cv.feature_set_name = "cross_venue_core";
    cv.feature_set_version = 2;
    cv.values = {100.0F, 100.5F, 100.2F};   // mid, mark, index -- three distinct prices
    cv.features.valid = true;
    asm_.ingest_alttech(cv);
    ParsedAltTech reg;
    reg.kind = AltTechKind::global_regime;
    reg.canonical_pair_id = 4;
    reg.regime.regime = "RANGE";
    asm_.ingest_alttech(reg);

    auto ctx = asm_.try_build(4, 1'000'100);
    REQUIRE(ctx.has_value());
    CHECK(ctx->mark_price == Catch::Approx(100.0));    // the mid: what last_price() wants
    CHECK(ctx->index_price == Catch::Approx(100.2));   // assigned at last
    CHECK(ctx->last_price() == Catch::Approx(100.0));
    // The mark stays reachable as its own member, so a basis is (mark - index)
    // rather than (mid - index): +29.9 bps here, not -19.96 bps.
    CHECK(ctx->cross_venue_features.get("consensus_mark_price", 0.0) == Catch::Approx(100.5));
}

TEST_CASE("crowding reads accumulated OI growth, not the one-minute delta") {
    // AltTech publishes only (OI[t] - OI[t-1min]) / OI[t-1min]. The crowding
    // hypothesis is about positioning built up over hours, and the 0.08 gate was
    // written for that: against the delta its 7-day maximum is 0.0616, so the
    // gate never opened. The threshold is unchanged here -- the input is.
    AssemblerConfig cfg;
    cfg.bitget_features = test_set();
    cfg.cross_venue_features.name = "cross_venue_core";
    cfg.cross_venue_features.version = 2;
    cfg.cross_venue_features.value_count = 1;
    cfg.cross_venue_features.positions = {{"oi_growth", 0}};

    const auto cv_msg = [](int64_t when, float delta) {
        ParsedAltTech m;
        m.kind = AltTechKind::cross_venue_features;
        m.canonical_pair_id = 4;
        m.bar_open_time_ms = when;
        m.feature_set_name = "cross_venue_core";
        m.feature_set_version = 2;
        m.values = {delta};
        m.features.valid = true;
        return m;
    };
    const auto build = [&](ContextAssembler& a, int64_t now) {
        // The execution bar has to be current at `now`, or try_build() declines
        // on component age before it ever looks at the OI window.
        auto bar = bitget_message(4, {1.0f, 2.0f, 3.0f, 4.0f, 5.0f});
        bar.bar_open_time_ms = now - 15LL * 60 * 1000;   // one closed 15m bar back
        a.ingest_alttech(bar);
        ParsedAltTech reg;
        reg.kind = AltTechKind::global_regime;
        reg.canonical_pair_id = 4;
        reg.regime.regime = "TREND_UP";
        a.ingest_alttech(reg);
        return a.try_build(4, now);
    };

    SECTION("too few minutes observed leaves it absent, never understated") {
        ContextAssembler a(cfg);
        const int64_t t0 = 10'000'000;
        for (int i = 0; i < 20; ++i) a.ingest_alttech(cv_msg(t0 + i * 60'000, 0.002F));
        auto ctx = build(a, t0 + 20 * 60'000);
        REQUIRE(ctx.has_value());
        CHECK_FALSE(ctx->cross_venue_features.has("oi_growth_1h"));
    }

    SECTION("a full window compounds the deltas") {
        ContextAssembler a(cfg);
        const int64_t t0 = 10'000'000;
        // 60 minutes of +0.2% each: (1.002)^60 - 1 = 0.127362
        for (int i = 0; i < 60; ++i) a.ingest_alttech(cv_msg(t0 + i * 60'000, 0.002F));
        auto ctx = build(a, t0 + 60 * 60'000);
        REQUIRE(ctx.has_value());
        REQUIRE(ctx->cross_venue_features.has("oi_growth_1h"));
        CHECK(ctx->cross_venue_features.get("oi_growth_1h") == Catch::Approx(0.127362).margin(1e-4));
        // The raw delta is still its own number and is NOT the accumulated one.
        CHECK(ctx->cross_venue_features.get("oi_growth") == Catch::Approx(0.002));
    }

    SECTION("a re-emitted bar replaces its earlier value rather than compounding twice") {
        ContextAssembler a(cfg);
        const int64_t t0 = 10'000'000;
        for (int i = 0; i < 60; ++i) a.ingest_alttech(cv_msg(t0 + i * 60'000, 0.002F));
        // AltTech re-emits a bar when a late venue arrives; the same minute must
        // not be counted a second time.
        a.ingest_alttech(cv_msg(t0 + 59 * 60'000, 0.002F));
        auto ctx = build(a, t0 + 60 * 60'000);
        REQUIRE(ctx.has_value());
        REQUIRE(ctx->cross_venue_features.has("oi_growth_1h"));
        CHECK(ctx->cross_venue_features.get("oi_growth_1h") == Catch::Approx(0.127362).margin(1e-4));
    }
}

TEST_CASE("a stale microstructure window does not enter the context") {
    // The higher grids are age-checked and skipped with a reason. This block was
    // copied unconditionally, so a stalled feed would keep serving the last CVD,
    // trade imbalance and book reading into every decision -- and downstream that
    // is indistinguishable from a market that went quiet.
    AssemblerConfig cfg;
    cfg.bitget_features = test_set();
    cfg.max_microstructure_age_ms = 120'000;
    cfg.microstructure_window = "1m";
    cfg.microstructure_features.name = "bitget_micro";
    cfg.microstructure_features.version = 1;
    cfg.microstructure_features.schema_hash = "micro-hash";
    cfg.microstructure_features.value_count = 1;
    cfg.microstructure_features.positions = {{"cvd_delta", 0}};

    const auto ctx_at = [&](int64_t micro_at, int64_t now) {
        ContextAssembler a(cfg);
        a.ingest_alttech(bitget_message(4, {1.0f, 2.0f, 3.0f, 4.0f, 5.0f}));
        ParsedAltTech m;
        m.kind = AltTechKind::bitget_microstructure;
        m.canonical_pair_id = 4;
        m.window_label = "1m";
        m.bar_open_time_ms = micro_at;
        m.feature_set_name = "bitget_micro";
        m.feature_set_version = 1;
        m.schema_hash = "micro-hash";
        m.values = {1234.0F};
        m.features.valid = true;
        a.ingest_alttech(m);
        ParsedAltTech reg;
        reg.kind = AltTechKind::global_regime;
        reg.canonical_pair_id = 4;
        reg.regime.regime = "TREND_UP";
        a.ingest_alttech(reg);
        return a.try_build(4, now);
    };

    // Age runs from the window's CLOSE. AltTech publishes a 1m window about 121 s
    // after that window STARTS, so measuring from the start put every window
    // past a 120 s bound the moment it arrived -- the book vanished from every
    // context again, this time by way of the freshness check.
    auto fresh = ctx_at(1'000'000, 1'000'000 + 150'000);   // 90 s past the close
    REQUIRE(fresh.has_value());
    CHECK(fresh->microstructure.get("cvd_delta", -1.0) == Catch::Approx(1234.0));

    // Six minutes past the close against a two-minute bound: absent.
    auto stale = ctx_at(1'000'000, 1'000'000 + 360'000);
    REQUIRE(stale.has_value());
    CHECK_FALSE(stale->microstructure.has("cvd_delta"));
    CHECK_FALSE(stale->spread_observed);
}

TEST_CASE("microprice deviation is converted to basis points, and the book is fresh from its close") {
    // Two unit errors met in the same block. AltTech publishes
    // book_microprice_dev as (microprice - mid) / mid -- a RATIO -- and two
    // strategies read it under a comment saying "bps" against thresholds of 0.5
    // and 3.0, short by a factor of 10 000: the measured maximum ratio over two
    // hours was 0.000446, i.e. 4.46 bps, which would have passed.
    //
    // And the book's freshness was measured from the window START. AltTech
    // publishes a 1m window ~121 s after it starts, so against a 120 s bound the
    // book never counted as observed: spread_bps was the conservative 10.0
    // fallback in 351 of 351 contexts while the producer reported a median of
    // 1.59 bps.
    AssemblerConfig cfg;
    cfg.bitget_features = test_set();
    cfg.max_microstructure_age_ms = 120'000;
    cfg.microstructure_window = "1m";
    cfg.microstructure_features.name = "bitget_micro";
    cfg.microstructure_features.version = 1;
    cfg.microstructure_features.schema_hash = "micro-hash";
    cfg.microstructure_features.value_count = 4;
    cfg.microstructure_features.positions = {{"spread_bps", 0},
                                             {"book_microprice_dev", 1},
                                             {"book_bid_depth", 2},
                                             {"book_ask_depth", 3}};

    ContextAssembler asm_(cfg);
    const int64_t window_start = 1'000'000;
    const int64_t decide_at = window_start + 60'000 + 90'000;   // 90 s past the close
    auto bar = bitget_message(4, {1.0f, 2.0f, 3.0f, 4.0f, 5.0f});
    bar.bar_open_time_ms = decide_at - 15LL * 60 * 1000;
    asm_.ingest_alttech(bar);
    ParsedAltTech m;
    m.kind = AltTechKind::bitget_microstructure;
    m.canonical_pair_id = 4;
    m.window_label = "1m";
    m.bar_open_time_ms = window_start;
    m.feature_set_name = "bitget_micro";
    m.feature_set_version = 1;
    m.schema_hash = "micro-hash";
    m.values = {1.59F, 0.000446F, 50'000.0F, 40'000.0F};
    m.features.valid = true;
    asm_.ingest_alttech(m);
    ParsedAltTech reg;
    reg.kind = AltTechKind::global_regime;
    reg.canonical_pair_id = 4;
    reg.regime.regime = "RANGE";
    asm_.ingest_alttech(reg);

    auto ctx = asm_.try_build(4, decide_at);
    REQUIRE(ctx.has_value());
    // The real spread, not the 10.0 stand-in.
    CHECK(ctx->spread_observed);
    CHECK(ctx->spread_bps == Catch::Approx(1.59).margin(0.01));
    CHECK(ctx->depth_observed);
    CHECK(ctx->top_depth_usdt == Catch::Approx(90'000.0));
    // The ratio is still itself, and the bps figure is derived beside it.
    CHECK(ctx->microstructure.get("book_microprice_dev") == Catch::Approx(0.000446).margin(1e-7));
    CHECK(ctx->microstructure.get("book_microprice_dev_bps") == Catch::Approx(4.46).margin(0.01));
}

TEST_CASE("derived composites come from pinned raw members, or not at all") {
    AssemblerConfig cfg;
    cfg.bitget_features.name = "talib-core";
    cfg.bitget_features.version = 1;
    cfg.bitget_features.schema_hash = "hash-A";
    cfg.bitget_features.value_count = 4;
    cfg.bitget_features.positions = {
        {"atr_14", 0}, {"atr_50", 1}, {"ema_20", 2}, {"ema_50", 3}};
    ContextAssembler asm_(cfg);

    // atr_14 = 3, atr_50 = 2, ema_20 = 101, ema_50 = 100
    auto msg = bitget_message(5, {3.0f, 2.0f, 101.0f, 100.0f});
    asm_.ingest_alttech(msg);
    ParsedAltTech reg;
    reg.kind = AltTechKind::global_regime;
    reg.canonical_pair_id = 5;
    reg.regime.regime = "TREND_UP";
    asm_.ingest_alttech(reg);

    auto ctx = asm_.try_build(5, 1'000'100);
    REQUIRE(ctx.has_value());
    CHECK(ctx->bitget_features.get("atr_expansion") == 1.5);   // 3 / 2
    // No price is available (no cross-venue mid, no microstructure), so the
    // price-relative composite is absent rather than divided by zero.
    CHECK_FALSE(ctx->bitget_features.has("ema_compression"));
    // Bollinger inputs were not pinned, so no bandwidth is invented.
    CHECK_FALSE(ctx->bitget_features.has("bb_width_20"));
}

TEST_CASE("snapshot columnar writer produces a stable checksummed file") {
    const std::string dir = (fs::temp_directory_path() / "altstrategy_snap_test").string();
    fs::create_directories(dir);
    ColumnarTable t;
    t.name = "signals";
    t.header = {"example_id", "label", "net_return"};
    t.rows = {{"ex1", "TARGET_FIRST", "0.004"}, {"ex2", "STOP_FIRST", "-0.006"},
              {"ex3", "AMBIGUOUS,with comma", "0"}};
    const auto f1 = write_table(dir, t, "csv");
    CHECK(f1.file_name == "signals.csv");
    CHECK(f1.row_count == 3);
    CHECK(f1.byte_size > 0);
    CHECK(f1.sha256.size() == 64);
    CHECK(fs::exists(dir + "/signals.csv"));
    // Deterministic: same content => same checksum.
    const auto f2 = write_table(dir, t, "csv");
    CHECK(f1.sha256 == f2.sha256);
    // RFC-4180 quoting of the comma-containing field.
    std::ifstream in(dir + "/signals.csv");
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(content.find("\"AMBIGUOUS,with comma\"") != std::string::npos);
    fs::remove_all(dir);
}

TEST_CASE("snapshot manifest json records identity, watermark and files") {
    SnapshotResult r;
    r.snapshot_id = "snap_abc";
    r.kind = "FULL";
    r.format = "csv";
    r.to_sequence = 128;
    r.signal_count = 42;
    r.files.push_back({"signals.csv", 42, 1024, "deadbeef"});
    const auto json = build_manifest_json(r, 1234567890);
    CHECK(json.find("\"snapshot_id\": \"snap_abc\"") != std::string::npos);
    CHECK(json.find("\"kind\": \"FULL\"") != std::string::npos);
    CHECK(json.find("\"to_sequence\": 128") != std::string::npos);
    CHECK(json.find("signals.csv") != std::string::npos);
    CHECK(json.find("deadbeef") != std::string::npos);
}

// ------------------------------------------- information venues

namespace {
ParsedAltTech venue_message(int64_t pair, int32_t venue, float fill, int32_t timeframe = 15) {
    ParsedAltTech p;
    p.kind = AltTechKind::reference_features;
    p.venue = venue;
    p.canonical_pair_id = pair;
    p.bar_open_time_ms = 1'000'000;
    p.revision = 3;
    p.timeframe_minutes = timeframe;
    p.feature_set_name = "talib-core";
    p.feature_set_version = 1;
    p.schema_hash = "hash-A";
    p.values.assign(5, fill);
    p.features.valid = true;
    return p;
}
} // namespace

TEST_CASE("each information venue keeps its own reading") {
    // Before this, two `exchange() != EXCHANGE_BITGET` gates in the codec turned
    // every reference vector into `none`, so the context declared a
    // reference_features summary that nothing could fill. Now each venue lands
    // under its own key -- and must NOT land under the target's.
    AssemblerConfig cfg;
    cfg.bitget_features = test_set();
    ContextAssembler a(cfg);
    a.ingest_alttech(bitget_message(7, std::vector<float>(5, 1.0f)));
    a.ingest_alttech(venue_message(7, 2, 2.0f));   // Binance
    a.ingest_alttech(venue_message(7, 3, 3.0f));   // OKX
    a.ingest_alttech(venue_message(7, 4, 4.0f));   // Bybit

    ParsedAltTech reg;
    reg.kind = AltTechKind::global_regime;
    reg.canonical_pair_id = 7;
    reg.regime.regime = "TREND_UP";
    reg.regime.confidence = 0.7;
    a.ingest_alttech(reg);

    const auto ctx = a.try_build(7, 1'000'100);
    REQUIRE(ctx.has_value());
    CHECK(ctx->reference_features.size() == 3);
    CHECK(ctx->reference_features.count(2) == 1);
    CHECK(ctx->reference_features.count(3) == 1);
    CHECK(ctx->reference_features.count(4) == 1);
    // The target venue is not a reference: merging it in would double-count the
    // very market the decision is about.
    CHECK(ctx->reference_features.count(1) == 0);

    // Venue identity is preserved value-for-value, not merged.
    const double b = ctx->reference_features.at(2).values.begin()->second;
    const double o = ctx->reference_features.at(3).values.begin()->second;
    const double y = ctx->reference_features.at(4).values.begin()->second;
    CHECK(b != o);
    CHECK(o != y);
    // And the target's own reading is untouched by any of them.
    CHECK(ctx->bitget_features.values.begin()->second == 1.0);
}

TEST_CASE("a reference vector on another timeframe does not enter the decision grid") {
    AssemblerConfig cfg;
    cfg.bitget_features = test_set();
    ContextAssembler a(cfg);
    a.ingest_alttech(bitget_message(7, std::vector<float>(5, 1.0f)));
    a.ingest_alttech(venue_message(7, 2, 2.0f, /*timeframe=*/60));  // 1h, not the grid

    ParsedAltTech reg;
    reg.kind = AltTechKind::global_regime;
    reg.canonical_pair_id = 7;
    reg.regime.regime = "TREND_UP";
    reg.regime.confidence = 0.7;
    a.ingest_alttech(reg);

    const auto ctx = a.try_build(7, 1'000'100);
    REQUIRE(ctx.has_value());
    CHECK(ctx->reference_features.empty());
}

TEST_CASE("freshness is measured from the bar's close, not its open") {
    // Reproduces the production failure directly. On a 15m decision grid with a
    // 20-minute component bound, a bar that opened 22 minutes ago closed 7
    // minutes ago -- its information is 7 minutes old and plainly usable. The
    // old rule compared age-from-OPEN against the bound, so it declined, and on
    // a 15m grid age-from-open starts at 15 minutes before the bar is even
    // observable. Measured on the test VPS: 2 294 declines in two hours for this
    // reason alone, with a healthy feed and no schema mismatch.
    AssemblerConfig cfg;
    cfg.bitget_features = test_set();
    cfg.decision_timeframe_minutes = 15;
    cfg.max_component_age_ms = 20 * 60 * 1000;
    ContextAssembler a(cfg);

    const int64_t bar_open = 10'000'000;
    auto msg = bitget_message(7, {100.0f, 0.99f, 0.97f, 1.0f, 28.0f}, /*timeframe=*/15);
    msg.bar_open_time_ms = bar_open;
    msg.features.bar_open_time_ms = bar_open;
    a.ingest_alttech(msg);

    ParsedAltTech reg;
    reg.kind = AltTechKind::global_regime;
    reg.canonical_pair_id = 7;
    reg.regime.regime = "TREND_UP";
    reg.regime.confidence = 0.7;
    a.ingest_alttech(reg);

    // 22 minutes after the open == 7 minutes after the close.
    const auto ctx = a.try_build(7, bar_open + 22 * 60 * 1000);
    REQUIRE(ctx.has_value());
    CHECK(ctx->bitget_features.get("px") == 100.0);

    // And a genuinely stale bar is still refused: 40 minutes after the open is
    // 25 after the close, past the 20-minute bound.
    ContextAssembler b(cfg);
    b.ingest_alttech(msg);
    b.ingest_alttech(reg);
    CHECK_FALSE(b.try_build(7, bar_open + 40 * 60 * 1000).has_value());
}

// ------------------------------------------- multi-timeframe context

namespace {
ParsedAltTech tf_message(int64_t pair, int32_t tf_minutes, int64_t bar_open, float fill) {
    ParsedAltTech p;
    p.kind = AltTechKind::bitget_features;
    p.venue = 1;
    p.canonical_pair_id = pair;
    p.bar_open_time_ms = bar_open;
    p.features.bar_open_time_ms = bar_open;
    p.revision = 3;
    p.timeframe_minutes = tf_minutes;
    p.feature_set_name = "talib-core";
    p.feature_set_version = 1;
    p.schema_hash = "hash-A";
    p.values.assign(5, fill);
    p.features.valid = true;
    return p;
}

AssemblerConfig mtf_config() {
    AssemblerConfig cfg;
    cfg.bitget_features = test_set();
    cfg.decision_timeframe_minutes = 15;
    cfg.context_timeframes_minutes = {30, 60, 240};
    cfg.max_component_age_ms = 60LL * 60 * 1000;
    cfg.max_context_age_ms = 6LL * 3600 * 1000;
    return cfg;
}

ParsedAltTech regime_for(int64_t pair) {
    ParsedAltTech r;
    r.kind = AltTechKind::global_regime;
    r.canonical_pair_id = pair;
    r.regime.regime = "TREND_UP";
    r.regime.confidence = 0.7;
    return r;
}
} // namespace

TEST_CASE("higher timeframes enter the context on their own keys") {
    ContextAssembler a(mtf_config());
    const int64_t t0 = 10'000'000;
    // Execution grid: a 15m bar that closed exactly at T0.
    a.ingest_alttech(tf_message(7, 15, t0 - 15 * 60'000, 1.0f));
    // Context grids, all closed before T0.
    a.ingest_alttech(tf_message(7, 30, t0 - 30 * 60'000, 2.0f));
    a.ingest_alttech(tf_message(7, 60, t0 - 60 * 60'000, 3.0f));
    a.ingest_alttech(tf_message(7, 240, t0 - 240 * 60'000, 4.0f));
    a.ingest_alttech(regime_for(7));

    const auto ctx = a.try_build(7, t0);
    REQUIRE(ctx.has_value());
    CHECK(ctx->context_features.count(30) == 1);
    CHECK(ctx->context_features.count(60) == 1);
    CHECK(ctx->context_features.count(240) == 1);
    // The execution grid is NOT duplicated into the context map.
    CHECK(ctx->context_features.count(15) == 0);
    // Each grid keeps its own values -- no merging, no substitution.
    CHECK(ctx->context_features.at(30).values.begin()->second == 2.0);
    CHECK(ctx->context_features.at(60).values.begin()->second == 3.0);
    CHECK(ctx->bitget_features.values.begin()->second == 1.0);
}

TEST_CASE("a higher-timeframe bar that has not closed cannot enter the context") {
    ContextAssembler a(mtf_config());
    const int64_t t0 = 10'000'000;
    a.ingest_alttech(tf_message(7, 15, t0 - 15 * 60'000, 1.0f));
    // A 1h bar that opened 30 minutes ago closes 30 minutes AFTER T0. Using it
    // would hand the strategy half of a bar that has not finished forming.
    a.ingest_alttech(tf_message(7, 60, t0 - 30 * 60'000, 9.0f));
    a.ingest_alttech(regime_for(7));

    const auto ctx = a.try_build(7, t0);
    REQUIRE(ctx.has_value());
    CHECK(ctx->context_features.count(60) == 0);
    // Absent, not substituted from the execution grid.
    CHECK(ctx->context_features.empty());
}

TEST_CASE("a stale higher timeframe is dropped rather than reused") {
    ContextAssembler a(mtf_config());
    const int64_t t0 = 10'000'000;
    a.ingest_alttech(tf_message(7, 15, t0 - 15 * 60'000, 1.0f));
    // A 4h bar that closed 7 hours ago, past the 6-hour context bound.
    a.ingest_alttech(tf_message(7, 240, t0 - (7 * 60 + 240) * 60'000, 4.0f));
    a.ingest_alttech(regime_for(7));

    const auto ctx = a.try_build(7, t0);
    REQUIRE(ctx.has_value());
    CHECK(ctx->context_features.count(240) == 0);
}

TEST_CASE("an undeclared timeframe never reaches the context") {
    ContextAssembler a(mtf_config());   // declares 30, 60, 240 only
    const int64_t t0 = 10'000'000;
    a.ingest_alttech(tf_message(7, 15, t0 - 15 * 60'000, 1.0f));
    a.ingest_alttech(tf_message(7, 1440, t0 - 1440 * 60'000, 5.0f));  // 1d, not declared
    a.ingest_alttech(regime_for(7));

    const auto ctx = a.try_build(7, t0);
    REQUIRE(ctx.has_value());
    CHECK(ctx->context_features.count(1440) == 0);
}

TEST_CASE("the VWAP reclaim is observed across bars, in basis points") {
    AssemblerConfig cfg;
    cfg.bitget_features = test_set();
    cfg.microstructure_features.name = "bitget_micro";
    cfg.microstructure_features.version = 1;
    cfg.microstructure_features.schema_hash = "micro-hash";
    cfg.microstructure_features.value_count = 2;
    // The book mid is the price; VWAP is the traded average. Feeding only VWAP
    // would make last_price() fall back to it and the deviation is then 0 by
    // construction -- which is exactly the shape of the bug being pinned.
    cfg.microstructure_features.positions = {{"vwap", 0}, {"book_mid", 1}};

    ContextAssembler asm_(cfg);
    ParsedAltTech reg;
    reg.kind = AltTechKind::global_regime;
    reg.canonical_pair_id = 11;
    reg.regime.regime = "TREND_UP";
    asm_.ingest_alttech(reg);

    // One 15-minute execution bar: mark price `px`, session VWAP `vwap`.
    const auto bar = [&](int64_t open_ms, float px, float vwap) {
        auto b = bitget_message(11, {px, 2.0f, 3.0f, 4.0f, 5.0f});
        b.bar_open_time_ms = open_ms;
        asm_.ingest_alttech(b);
        ParsedAltTech micro;
        micro.kind = AltTechKind::bitget_microstructure;
        micro.window_label = "1m";
        micro.canonical_pair_id = 11;
        // The microstructure window is published every minute, so at decision
        // time the newest one covers the minute just gone -- not the minute the
        // 15m bar opened, which would be a quarter of an hour stale and is now
        // (correctly) refused on age.
        const int64_t decide_at = open_ms + 15 * 60'000 + 100;
        micro.bar_open_time_ms = decide_at - 60'000;
        micro.feature_set_name = "bitget_micro";
        micro.feature_set_version = 1;
        micro.schema_hash = "micro-hash";
        micro.values = {vwap, px};
        micro.features.valid = true;
        asm_.ingest_alttech(micro);
        return asm_.try_build(11, decide_at);
    };

    constexpr int64_t kBar = 15 * 60'000;
    const int64_t t0 = 1'000'000;

    // Bar 0: price 20 bps BELOW vwap. No crossing has been seen yet, so the
    // question "did price reclaim?" has no answer -- and no answer is absent,
    // not a zero that reads as "no".
    auto c0 = bar(t0, 99.8F, 100.0F);
    REQUIRE(c0.has_value());
    CHECK(c0->bitget_features.get("vwap_dev", 999.0) == Catch::Approx(-20.0).margin(0.05));
    CHECK_FALSE(c0->bitget_features.has("vwap_lost_recently"));

    // Bar 1: price crosses back above vwap. That is the reclaim.
    auto c1 = bar(t0 + kBar, 100.2F, 100.0F);
    REQUIRE(c1.has_value());
    CHECK(c1->bitget_features.get("vwap_dev", 999.0) == Catch::Approx(20.0).margin(0.05));
    CHECK(c1->bitget_features.get("vwap_lost_recently", 0.0) == 1.0);

    // Still within the five-bar window.
    auto c2 = bar(t0 + 5 * kBar, 100.3F, 100.0F);
    REQUIRE(c2.has_value());
    CHECK(c2->bitget_features.get("vwap_lost_recently", 0.0) == 1.0);

    // Past it: price above VWAP for six bars is a trend, not a reclaim.
    auto c3 = bar(t0 + 7 * kBar, 100.4F, 100.0F);
    REQUIRE(c3.has_value());
    CHECK(c3->bitget_features.get("vwap_lost_recently", 1.0) == 0.0);
}

TEST_CASE("the OI window holds a full hour whatever order the minutes arrive in") {
    // AltTech's cross-venue sweep computes the NEWEST uncomputed minute first
    // and fills the gaps behind it later, so minutes arrive newest-first as
    // often as not. The first version popped every held entry at or after an
    // incoming bar: a minute arriving behind a newer one emptied the window
    // down to itself, and oi_growth_1h never reached its 45 samples.
    const int64_t t0 = 10'000'000;
    const auto minute = [&](int i) { return t0 + static_cast<int64_t>(i) * 60'000; };

    SECTION("newest-first arrival keeps every minute") {
        std::deque<std::pair<int64_t, double>> q;
        for (int i = 59; i >= 0; --i) record_oi_change(q, minute(i), 0.001);
        REQUIRE(q.size() == 60);
        for (size_t k = 1; k < q.size(); ++k) CHECK(q[k - 1].first < q[k].first);
    }

    SECTION("a gap filled later lands in order and discards nothing") {
        std::deque<std::pair<int64_t, double>> q;
        for (int i = 0; i < 60; i += 2) record_oi_change(q, minute(i), 0.001);   // evens
        for (int i = 1; i < 60; i += 2) record_oi_change(q, minute(i), 0.002);   // odds, later
        REQUIRE(q.size() == 60);
        for (size_t k = 1; k < q.size(); ++k) CHECK(q[k - 1].first < q[k].first);
    }

    SECTION("a repeat of an old minute replaces only that minute") {
        std::deque<std::pair<int64_t, double>> q;
        for (int i = 0; i < 60; ++i) record_oi_change(q, minute(i), 0.001);
        record_oi_change(q, minute(39), 0.005);   // twenty minutes back
        REQUIRE(q.size() == 60);
        CHECK(q[39].second == 0.005);
        CHECK(q.back().first == minute(59));   // the newer minutes survive
    }

    SECTION("the window is trimmed against the newest minute held, not the incoming one") {
        std::deque<std::pair<int64_t, double>> q;
        for (int i = 0; i < 60; ++i) record_oi_change(q, minute(i), 0.001);
        // A late minute from before the window arrives: it is older than
        // newest - 1h, so it cannot stay, and it takes nothing with it.
        record_oi_change(q, minute(-5), 0.001);
        CHECK(q.size() == 60);
        CHECK(q.front().first == minute(0));
        // A new minute moves the window forward by exactly one.
        record_oi_change(q, minute(60), 0.001);
        CHECK(q.size() == 60);
        CHECK(q.front().first == minute(1));
    }
}

TEST_CASE("a backfilled cross-venue minute never replaces a newer one") {
    // consensus_mid_price -- the price every plan is built from -- is read from
    // the cross-venue summary, and AltTech publishes backfilled minutes after
    // newer ones. Keeping the last to ARRIVE priced a decision up to the
    // sweep's lookback in the past.
    AssemblerConfig cfg;
    cfg.bitget_features = test_set();
    cfg.cross_venue_features.name = "cross_venue_core";
    cfg.cross_venue_features.version = 2;
    cfg.cross_venue_features.value_count = 1;
    cfg.cross_venue_features.positions = {{"consensus_mid_price", 0}};

    const auto cv_at = [](int64_t bar_ms, float mid) {
        ParsedAltTech cv;
        cv.kind = AltTechKind::cross_venue_features;
        cv.canonical_pair_id = 4;
        cv.bar_open_time_ms = bar_ms;
        cv.feature_set_name = "cross_venue_core";
        cv.feature_set_version = 2;
        cv.values = {mid};
        cv.features.valid = true;
        return cv;
    };
    const auto regime_at = [](int64_t bar_ms, const char* name) {
        ParsedAltTech reg;
        reg.kind = AltTechKind::global_regime;
        reg.canonical_pair_id = 4;
        reg.regime.regime = name;
        reg.regime.bar_open_time_ms = bar_ms;
        return reg;
    };

    ContextAssembler asm_(cfg);
    asm_.ingest_alttech(bitget_message(4, {1.0f, 2.0f, 3.0f, 4.0f, 5.0f}));
    asm_.ingest_alttech(cv_at(960'000, 100.0F));        // the current minute
    asm_.ingest_alttech(regime_at(960'000, "TREND_UP"));
    asm_.ingest_alttech(cv_at(900'000, 90.0F));         // a backfilled older minute
    asm_.ingest_alttech(regime_at(900'000, "RANGE"));

    auto ctx = asm_.try_build(4, 1'000'100);
    REQUIRE(ctx.has_value());
    CHECK(ctx->last_price() == Catch::Approx(100.0));
    CHECK(ctx->cross_venue_features.bar_open_time_ms == 960'000);
    CHECK(ctx->global_regime.regime == "TREND_UP");

    // A re-emit of the held minute (a late venue arrived) still replaces it.
    asm_.ingest_alttech(cv_at(960'000, 101.0F));
    auto next_bar = bitget_message(4, {1.0f, 2.0f, 3.0f, 4.0f, 5.0f});
    next_bar.bar_open_time_ms = 1'060'000;   // a newer execution bar: a new revision
    asm_.ingest_alttech(next_bar);
    auto again = asm_.try_build(4, 1'000'200);
    REQUIRE(again.has_value());
    CHECK(again->last_price() == Catch::Approx(101.0));
}

TEST_CASE("the session VWAP is the day's traded notional over its traded volume") {
    constexpr int64_t kDay = 86'400'000;
    constexpr int64_t kMin = 60'000;
    const int64_t d0 = 20'000 * kDay;   // a UTC midnight
    SessionVwap s;

    SECTION("exact weighting, and a window with no trade counts only toward coverage") {
        s.record(d0 + 0 * kMin, 100.0, 2.0);
        s.record(d0 + 1 * kMin, std::nullopt, std::nullopt);   // no trade that minute
        s.record(d0 + 2 * kMin, 103.0, 1.0);
        REQUIRE(s.value().has_value());
        CHECK(*s.value() == Catch::Approx((100.0 * 2 + 103.0 * 1) / 3.0));
        CHECK(s.windows == 3);
    }
    SECTION("a session whose open was not observed has no session VWAP") {
        // A restart at 00:10 would accumulate from 00:10 -- another anchor.
        for (int m = 10; m < 60; ++m) s.record(d0 + m * kMin, 100.0, 1.0);
        CHECK_FALSE(s.value().has_value());
    }
    SECTION("the open within the tolerance is the open") {
        for (int m = 5; m < 60; ++m) s.record(d0 + m * kMin, 100.0, 1.0);
        CHECK(s.value().has_value());
    }
    SECTION("coverage below 75% of the session's minutes is absent, 75% is present") {
        // 100 minutes elapsed; 75 observed at the boundary.
        for (int m = 0; m < 75; ++m) s.record(d0 + m * kMin, 100.0, 1.0);
        s.record(d0 + 99 * kMin, 100.0, 1.0);   // 76 of 100
        CHECK(s.value().has_value());
        SessionVwap t;
        for (int m = 0; m < 73; ++m) t.record(d0 + m * kMin, 100.0, 1.0);
        t.record(d0 + 99 * kMin, 100.0, 1.0);   // 74 of 100
        CHECK_FALSE(t.value().has_value());
    }
    SECTION("a repeated or late window is never counted twice") {
        s.record(d0, 100.0, 1.0);
        s.record(d0 + kMin, 110.0, 1.0);
        s.record(d0 + kMin, 110.0, 1.0);   // repeat
        s.record(d0, 100.0, 1.0);          // late
        CHECK(s.windows == 2);
        CHECK(*s.value() == Catch::Approx(105.0));
    }
    SECTION("a new UTC day starts a new session, and a window from the old one is ignored") {
        s.record(d0, 100.0, 1.0);
        s.record(d0 + kDay, 200.0, 1.0);
        CHECK(*s.value() == Catch::Approx(200.0));
        s.record(d0 + 10 * kMin, 50.0, 100.0);   // yesterday, arriving late
        CHECK(*s.value() == Catch::Approx(200.0));
        CHECK(s.windows == 1);
    }
}

TEST_CASE("the context carries the session VWAP accumulated from the 1m windows") {
    constexpr int64_t kDay = 86'400'000;
    constexpr int64_t kMin = 60'000;
    const int64_t d0 = 20'000 * kDay;
    AssemblerConfig cfg;
    cfg.bitget_features = test_set();
    cfg.decision_timeframe_minutes = 15;
    cfg.microstructure_window = "1m";
    cfg.microstructure_features.name = "bitget_micro";
    cfg.microstructure_features.version = 1;
    cfg.microstructure_features.schema_hash = "micro-hash";
    cfg.microstructure_features.value_count = 3;
    cfg.microstructure_features.positions = {
        {"vwap", 0}, {"aggressive_buy_volume", 1}, {"aggressive_sell_volume", 2}};
    cfg.cross_venue_features.name = "cross_venue_core";
    cfg.cross_venue_features.version = 2;
    cfg.cross_venue_features.value_count = 1;
    cfg.cross_venue_features.positions = {{"consensus_mid_price", 0}};
    ContextAssembler asm_(cfg);

    const auto window = [&](int64_t start, float vwap, float buy, float sell) {
        ParsedAltTech m;
        m.kind = AltTechKind::bitget_microstructure;
        m.window_label = "1m";
        m.canonical_pair_id = 5;
        m.bar_open_time_ms = start;
        m.feature_set_name = "bitget_micro";
        m.feature_set_version = 1;
        m.schema_hash = "micro-hash";
        m.values = {vwap, buy, sell};
        m.features.valid = true;
        asm_.ingest_alttech(m);
    };
    // 00:00 .. 00:29: first half at 100 (volume 2), second half at 103 (volume 1).
    for (int i = 0; i < 30; ++i) window(d0 + i * kMin, i < 15 ? 100.0F : 103.0F, i < 15 ? 1.5F : 0.5F, 0.5F);

    const int64_t bar_open = d0 + 15 * kMin;          // the 00:15 execution bar
    const int64_t now = d0 + 31 * kMin + 5'000;       // just after the 00:30 close
    auto b = bitget_message(5, {1.0f, 2.0f, 3.0f, 4.0f, 5.0f});
    b.bar_open_time_ms = bar_open;
    asm_.ingest_alttech(b);
    ParsedAltTech cv;
    cv.kind = AltTechKind::cross_venue_features;
    cv.canonical_pair_id = 5;
    cv.bar_open_time_ms = d0 + 30 * kMin;
    cv.feature_set_name = "cross_venue_core";
    cv.feature_set_version = 2;
    cv.values = {102.0F};
    cv.features.valid = true;
    asm_.ingest_alttech(cv);
    ParsedAltTech reg;
    reg.kind = AltTechKind::global_regime;
    reg.canonical_pair_id = 5;
    reg.regime.regime = "RANGE";
    asm_.ingest_alttech(reg);

    auto ctx = asm_.try_build(5, now);
    REQUIRE(ctx.has_value());
    const double expected = (100.0 * 2.0 * 15 + 103.0 * 1.0 * 15) / (2.0 * 15 + 1.0 * 15);   // 101.0
    REQUIRE(ctx->bitget_features.has("session_vwap"));
    CHECK(ctx->bitget_features.get("session_vwap") == Catch::Approx(expected));
    CHECK(ctx->bitget_features.get("session_vwap_dev_bps") ==
          Catch::Approx((102.0 - expected) / expected * 1e4).margin(1e-6));
    // The minute VWAP is still the minute's, under its own name.
    CHECK(ctx->bitget_features.get("vwap", 0.0) == Catch::Approx(103.0));

    // No window for longer than the microstructure freshness bound: the level
    // as of T0 is unknown, so it is absent rather than carried forward.
    auto b2 = bitget_message(5, {1.0f, 2.0f, 3.0f, 4.0f, 5.0f});
    b2.bar_open_time_ms = bar_open + 15 * kMin;
    asm_.ingest_alttech(b2);
    auto late = asm_.try_build(5, now + 15 * kMin);
    REQUIRE(late.has_value());
    CHECK_FALSE(late->bitget_features.has("session_vwap"));
}

TEST_CASE("the prior Donchian channel is the previous bar's, and only the previous bar's") {
    // Positions: px, donchian_high_20, donchian_low_20, atr_14, adx_14.
    PositionalFeatureSet set;
    set.name = "talib-core";
    set.version = 1;
    set.schema_hash = "hash-A";
    set.value_count = 5;
    set.positions = {{"px", 0}, {"donchian_high_20", 1}, {"donchian_low_20", 2}, {"atr_14", 3}, {"adx_14", 4}};
    AssemblerConfig cfg;
    cfg.bitget_features = set;
    cfg.decision_timeframe_minutes = 15;
    ContextAssembler asm_(cfg);
    constexpr int64_t kBar = 15 * 60'000;
    const int64_t t0 = 900'000'000;

    ParsedAltTech reg;
    reg.kind = AltTechKind::global_regime;
    reg.canonical_pair_id = 3;
    reg.regime.regime = "BREAKOUT";
    asm_.ingest_alttech(reg);
    const auto bar = [&](int64_t open, float hi, float lo, int64_t revision = 3) {
        auto b = bitget_message(3, {100.0f, hi, lo, 1.0f, 30.0f});
        b.bar_open_time_ms = open;
        b.revision = revision;
        asm_.ingest_alttech(b);
    };

    bar(t0, 101.0F, 95.0F);
    auto c0 = asm_.try_build(3, t0 + kBar + 1'000);
    REQUIRE(c0.has_value());
    CHECK_FALSE(c0->bitget_features.has("donchian_high_20_prev"));   // no previous bar yet

    bar(t0 + kBar, 102.0F, 95.0F);
    auto c1 = asm_.try_build(3, t0 + 2 * kBar + 1'000);
    REQUIRE(c1.has_value());
    CHECK(c1->bitget_features.get("donchian_high_20_prev", 0.0) == 101.0);
    CHECK(c1->bitget_features.get("donchian_high_20", 0.0) == 102.0);

    // A later revision of the same bar does not shift the previous slot, and an
    // older bar arriving late does not replace the current one.
    bar(t0 + kBar, 102.5F, 95.0F, 4);
    bar(t0, 90.0F, 80.0F, 9);
    bar(t0 + 2 * kBar, 103.0F, 95.0F);
    auto c2 = asm_.try_build(3, t0 + 3 * kBar + 1'000);
    REQUIRE(c2.has_value());
    CHECK(c2->bitget_features.get("donchian_high_20_prev", 0.0) == 102.5);

    // A gap: the channel from two bars ago is a different channel.
    bar(t0 + 4 * kBar, 104.0F, 95.0F);
    auto c3 = asm_.try_build(3, t0 + 5 * kBar + 1'000);
    REQUIRE(c3.has_value());
    CHECK_FALSE(c3->bitget_features.has("donchian_high_20_prev"));
}

TEST_CASE("cross-venue agreement is signed with the consensus direction") {
    AssemblerConfig cfg;
    cfg.bitget_features = test_set();
    cfg.cross_venue_features.name = "cross_venue_core";
    cfg.cross_venue_features.version = 2;
    cfg.cross_venue_features.value_count = 4;
    cfg.cross_venue_features.positions = {{"consensus_momentum_score", 0}, {"momentum_agreement", 1},
                                          {"consensus_flow", 2}, {"breakout_agreement", 3}};
    const auto build = [&](float score, float share, float flow, float fshare, int64_t bar) {
        ContextAssembler a(cfg);
        auto b = bitget_message(6, {1.0f, 2.0f, 3.0f, 4.0f, 5.0f});
        a.ingest_alttech(b);
        ParsedAltTech cv;
        cv.kind = AltTechKind::cross_venue_features;
        cv.canonical_pair_id = 6;
        cv.bar_open_time_ms = bar;
        cv.feature_set_name = "cross_venue_core";
        cv.feature_set_version = 2;
        cv.values = {score, share, flow, fshare};
        cv.features.valid = true;
        a.ingest_alttech(cv);
        ParsedAltTech reg;
        reg.kind = AltTechKind::global_regime;
        reg.canonical_pair_id = 6;
        reg.regime.regime = "RANGE";
        a.ingest_alttech(reg);
        return a.try_build(6, 1'000'100);
    };
    auto up = build(0.0004F, 0.75F, -2500.0F, 0.5F, 1'000'000);
    REQUIRE(up.has_value());
    CHECK(up->cross_venue_features.get("momentum_direction_agreement", 9) == Catch::Approx(0.75));
    CHECK(up->cross_venue_features.get("flow_direction_agreement", 9) == Catch::Approx(-0.5));
    CHECK(up->cross_venue_features.get("momentum_agreement", 9) == Catch::Approx(0.75));   // the share, unchanged
    auto flat = build(0.0F, 0.25F, 0.0F, 1.0F, 1'000'000);
    REQUIRE(flat.has_value());
    CHECK(flat->cross_venue_features.get("momentum_direction_agreement", 9) == 0.0);   // no direction, measured
    // No direction reading: the signed agreement is absent, not a guess.
    cfg.cross_venue_features.positions = {{"momentum_agreement", 1}, {"breakout_agreement", 3}};
    auto none = build(0.0004F, 0.75F, -2500.0F, 0.5F, 1'000'000);
    REQUIRE(none.has_value());
    CHECK_FALSE(none->cross_venue_features.has("momentum_direction_agreement"));
    CHECK_FALSE(none->cross_venue_features.has("flow_direction_agreement"));
}


TEST_CASE("unobserved context scalars are persisted as NULL, never as their stand-ins") {
    // The Gateway presence-gates these columns on NULL; a stand-in spread
    // written as a number reached the executor as a measured one.
    auto ctx = altstrategy::engine::test::make_ctx();
    ctx.spread_observed = false;   ctx.spread_bps = 10.0;        // stand-in
    ctx.depth_observed = true;     ctx.top_depth_usdt = 42'000;
    ctx.liquidity_rank = 0.0;                                     // absent
    ctx.funding_rate = 0.0;
    ctx.cross_venue_features.values.erase("funding_rate_8h");     // never arrived
    auto s = altstrategy::engine_io::observed_scalars(ctx);
    CHECK_FALSE(s.spread_bps.has_value());
    CHECK(s.top_depth_usdt == 42'000);
    CHECK_FALSE(s.liquidity_rank.has_value());
    CHECK_FALSE(s.funding_rate.has_value());

    ctx.spread_observed = true;  ctx.spread_bps = 1.6;
    ctx.liquidity_rank = 2.0;
    ctx.cross_venue_features.values["funding_rate_8h"] = 0.0;   // a MEASURED zero is a value
    s = altstrategy::engine_io::observed_scalars(ctx);
    CHECK(s.spread_bps == 1.6);
    CHECK(s.liquidity_rank == 2.0);
    REQUIRE(s.funding_rate.has_value());
    CHECK(*s.funding_rate == 0.0);
}

// ------------------------------------------- ML-first context inputs

namespace {
AssemblerConfig micro_cfg() {
    AssemblerConfig cfg;
    cfg.bitget_features = test_set();
    cfg.max_microstructure_age_ms = 120'000;
    cfg.microstructure_window = "1m";
    cfg.microstructure_features.name = "bitget_micro";
    cfg.microstructure_features.version = 1;
    cfg.microstructure_features.schema_hash = "micro-hash";
    cfg.microstructure_features.value_count = 1;
    cfg.microstructure_features.positions = {{"cvd_delta", 0}};
    return cfg;
}
ParsedAltTech micro_at(int64_t pair, int64_t window_start, float cvd) {
    ParsedAltTech m;
    m.kind = AltTechKind::bitget_microstructure;
    m.canonical_pair_id = pair;
    m.window_label = "1m";
    m.bar_open_time_ms = window_start;
    m.feature_set_name = "bitget_micro";
    m.feature_set_version = 1;
    m.schema_hash = "micro-hash";
    m.values = {cvd};
    m.features.valid = true;
    return m;
}
} // namespace

TEST_CASE("the microstructure race waits for the next window, never widens the bound",
          "[assembler][freshness]") {
    // The newest window a consumer can hold is 61..121 s old (AltTech computes
    // a 1m window ~61 s after it closes) against a 120 s bound. A build that
    // lands in that last second used to go out WITHOUT a book.
    ContextAssembler a(micro_cfg());
    a.ingest_alttech(bitget_message(4, {1.0f, 2.0f, 3.0f, 4.0f, 5.0f}));
    a.ingest_alttech(regime_for(4));
    const int64_t w = 1'000'000;
    a.ingest_alttech(micro_at(4, w, 111.0f));
    const int64_t now = w + 60'000 + 120'500;             // 120.5 s past the close
    CHECK_FALSE(a.try_build(4, now).has_value());          // held back, not built bookless
    CHECK(a.awaiting_microstructure(4));
    // The next window lands; the build completes with the FRESH window.
    a.ingest_alttech(micro_at(4, w + 60'000, 222.0f));
    const auto ctx = a.try_build(4, now + 800);
    REQUIRE(ctx.has_value());
    CHECK(ctx->microstructure.get("cvd_delta", -1.0) == Catch::Approx(222.0));
    CHECK_FALSE(a.awaiting_microstructure(4));
}

TEST_CASE("a window that never comes does not hold the context forever", "[assembler][freshness]") {
    ContextAssembler a(micro_cfg());
    a.ingest_alttech(bitget_message(4, {1.0f, 2.0f, 3.0f, 4.0f, 5.0f}));
    a.ingest_alttech(regime_for(4));
    const int64_t w = 1'000'000;
    a.ingest_alttech(micro_at(4, w, 111.0f));
    const int64_t now = w + 60'000 + 121'000;
    CHECK_FALSE(a.try_build(4, now).has_value());
    // After the grace the context is built -- without the stale book.
    const auto ctx = a.try_build(4, now + ContextAssembler::kMicroWaitGraceMs);
    REQUIRE(ctx.has_value());
    CHECK_FALSE(ctx->microstructure.has("cvd_delta"));
    CHECK_FALSE(ctx->spread_observed);
}

TEST_CASE("econ estimates enter per horizon, newest bar, only while fresh", "[assembler][ml]") {
    AssemblerConfig cfg;
    cfg.bitget_features = test_set();
    cfg.max_forecast_age_ms = 180'000;
    ContextAssembler a(cfg);
    a.ingest_alttech(bitget_message(4, {1.0f, 2.0f, 3.0f, 4.0f, 5.0f}));
    a.ingest_alttech(regime_for(4));
    const int64_t now = 1'200'000;
    const auto fc = [&](int32_t h, int64_t bar, double net_long) {
        ParsedForecast f;
        f.canonical_pair_id = 4;
        f.view = altstrategy::engine::test::econ_forecast(h, net_long, -30.0, bar);
        a.ingest_forecast(f);
    };
    fc(15, now - 120'000, 1.0);        // closed 60 s ago: fresh
    fc(15, now - 180'000, 9.0);        // an OLDER bar arriving late never replaces it
    fc(60, now - 300'000, 2.0);        // closed 240 s ago: stale
    fc(240, now + 60'000, 3.0);        // a bar that has not closed: impossible, refused
    const auto ctx = a.try_build(4, now);
    REQUIRE(ctx.has_value());
    REQUIRE(ctx->forecasts.count(15) == 1);
    CHECK(ctx->forecasts.at(15).expected_net_long_bps == Catch::Approx(1.0));
    CHECK(ctx->forecasts.count(60) == 0);
    CHECK(ctx->forecasts.count(240) == 0);
}

TEST_CASE("Bitget 15m / 1h regimes enter on their own keys while fresh", "[assembler][mtf]") {
    AssemblerConfig cfg;
    cfg.bitget_features = test_set();
    ContextAssembler a(cfg);
    const int64_t now = 10'000'000;
    auto bm = bitget_message(4, {1.0f, 2.0f, 3.0f, 4.0f, 5.0f});
    bm.bar_open_time_ms = now - 20 * 60'000;
    bm.features.bar_open_time_ms = bm.bar_open_time_ms;
    a.ingest_alttech(bm);
    ParsedAltTech onem;
    onem.kind = AltTechKind::bitget_regime;          // the 1m venue_lite regime
    onem.canonical_pair_id = 4;
    onem.regime.regime = "TREND_DOWN";
    a.ingest_alttech(onem);
    ParsedAltTech htf;
    htf.kind = AltTechKind::bitget_htf_regime;
    htf.canonical_pair_id = 4;
    RegimeView r15; r15.regime = "RANGE"; r15.bar_open_time_ms = now - 20 * 60'000;     // closed 5 min ago
    RegimeView r60; r60.regime = "TREND_UP"; r60.bar_open_time_ms = now - 4 * 3'600'000; // closed 3 h ago: stale
    htf.htf_regimes = {{15, r15}, {60, r60}};
    a.ingest_alttech(htf);
    const auto ctx = a.try_build(4, now);
    REQUIRE(ctx.has_value());
    CHECK(ctx->bitget_regime.regime == "TREND_DOWN");        // the 1m one, untouched
    REQUIRE(ctx->bitget_htf_regime.count(15) == 1);
    CHECK(ctx->bitget_htf_regime.at(15).regime == "RANGE");
    CHECK(ctx->bitget_htf_regime.count(60) == 0);             // stale: absent, not reused
}

TEST_CASE("the 15m venue agreement is computed on the decision grid", "[assembler][mtf]") {
    AssemblerConfig cfg;
    cfg.bitget_features = test_set();
    cfg.bitget_features.positions = {{"px", 0}, {"ema_20", 1}, {"ema_50", 2}, {"atr_14", 3}, {"roc_5", 4}};
    cfg.decision_timeframe_minutes = 15;
    cfg.max_component_age_ms = 60LL * 60 * 1000;
    const int64_t bar = 1'000'000 - 15 * 60'000;
    const auto build = [&](std::vector<float> ref_rocs) {
        ContextAssembler a(cfg);
        auto b = bitget_message(4, {1.0f, 2.0f, 3.0f, 4.0f, 0.5f});   // Bitget roc_5 up
        b.bar_open_time_ms = bar;
        b.features.bar_open_time_ms = bar;
        a.ingest_alttech(b);
        a.ingest_alttech(regime_for(4));
        int32_t venue = 2;
        for (const float r : ref_rocs) {
            auto m = bitget_message(4, {1.0f, 2.0f, 3.0f, 4.0f, r});
            m.kind = AltTechKind::reference_features;
            m.venue = venue++;
            m.bar_open_time_ms = bar;
            m.features.bar_open_time_ms = bar;
            a.ingest_alttech(m);
        }
        return a.try_build(4, 1'000'000 + 60'000);
    };
    const auto all_up = build({0.3f, 0.2f, 0.4f});
    REQUIRE(all_up.has_value());
    CHECK(all_up->bitget_features.get("momentum_direction_agreement_15m", -9) == Catch::Approx(1.0));
    const auto mixed = build({-0.3f, 0.2f, -0.1f});           // consensus up, two of four agree
    REQUIRE(mixed.has_value());
    CHECK(mixed->bitget_features.get("momentum_direction_agreement_15m", -9) == Catch::Approx(0.5));
    const auto down = build({-0.9f, -0.2f, -0.4f});           // consensus down, Bitget alone up
    REQUIRE(down.has_value());
    CHECK(down->bitget_features.get("momentum_direction_agreement_15m", -9) == Catch::Approx(-0.75));
    // Fewer than three venues observed: the build waits for the decision
    // bar's venues first (see the next test); built without them, the
    // agreement is absent, not "no agreement".
    const auto thin = build({0.3f});
    CHECK_FALSE(thin.has_value());
}

TEST_CASE("the decision bar waits for its information venues, then gives up", "[assembler][mtf]") {
    AssemblerConfig cfg;
    cfg.bitget_features = test_set();
    cfg.bitget_features.positions = {{"px", 0}, {"ema_20", 1}, {"ema_50", 2}, {"atr_14", 3}, {"roc_5", 4}};
    cfg.decision_timeframe_minutes = 15;
    cfg.max_component_age_ms = 60LL * 60 * 1000;
    const int64_t bar = 1'000'000 - 15 * 60'000;
    const auto venue = [&](int32_t v, int64_t bar_ms, float roc) {
        auto m = bitget_message(4, {1.0f, 2.0f, 3.0f, 4.0f, roc});
        m.kind = AltTechKind::reference_features;
        m.venue = v;
        m.bar_open_time_ms = bar_ms;
        m.features.bar_open_time_ms = bar_ms;
        return m;
    };
    const int64_t now = 1'000'000 + 60'000;
    ContextAssembler a(cfg);
    // The venues' PREVIOUS bar is what is held when Bitget's new bar arrives.
    for (int32_t v : {2, 3, 4}) a.ingest_alttech(venue(v, bar - 15 * 60'000, 0.2f));
    auto b = bitget_message(4, {1.0f, 2.0f, 3.0f, 4.0f, 0.5f});
    b.bar_open_time_ms = bar;
    b.features.bar_open_time_ms = bar;
    a.ingest_alttech(b);
    a.ingest_alttech(regime_for(4));
    CHECK_FALSE(a.try_build(4, now).has_value());
    CHECK(a.awaiting_information_venues(4));
    // Two venues of the decision bar arrive 15 s later: built, with the agreement.
    a.ingest_alttech(venue(2, bar, 0.3f));
    CHECK_FALSE(a.try_build(4, now + 10'000).has_value());   // one is not enough
    a.ingest_alttech(venue(3, bar, 0.4f));
    const auto ctx = a.try_build(4, now + 15'000);
    REQUIRE(ctx.has_value());
    CHECK(ctx->bitget_features.get("momentum_direction_agreement_15m", -9) == Catch::Approx(1.0));
    CHECK_FALSE(a.awaiting_information_venues(4));

    // Venues that never arrive: after the wait the context is built without them.
    ContextAssembler c(cfg);
    for (int32_t v : {2, 3, 4}) c.ingest_alttech(venue(v, bar - 15 * 60'000, 0.2f));
    c.ingest_alttech(b);
    c.ingest_alttech(regime_for(4));
    CHECK_FALSE(c.try_build(4, now).has_value());
    const auto late = c.try_build(4, now + ContextAssembler::kReferenceWaitMs);
    REQUIRE(late.has_value());
    CHECK_FALSE(late->bitget_features.has("momentum_direction_agreement_15m"));
}

TEST_CASE("the session VWAP survives a restart: rebuilt from the day's candles", "[assembler][vwap]") {
    AssemblerConfig cfg;
    cfg.bitget_features = test_set();
    cfg.microstructure_window = "1m";
    ContextAssembler a(cfg);
    const int64_t day = 20'000LL * 86'400'000;            // a UTC midnight
    const int64_t first_live = day + 600 * 60'000;         // restarted at 10:00
    // The first live window asks for a seed from the session start.
    const auto need = a.session_vwap_seed_needed(4, first_live, "1m");
    REQUIRE(need.has_value());
    CHECK(*need == day);
    CHECK_FALSE(a.session_vwap_seed_needed(4, first_live, "1s").has_value());   // not the pinned window
    CHECK_FALSE(a.session_vwap_seed_needed(4, day, "1m").has_value());          // the session's own first minute
    // 600 candles: price 100 on volume 1, then price 110 on volume 3.
    std::vector<SessionMinute> m;
    for (int i = 0; i < 600; ++i) {
        const double px = i < 300 ? 100.0 : 110.0, vol = i < 300 ? 1.0 : 3.0;
        m.push_back({day + i * 60'000LL, px * vol, vol});
    }
    a.seed_session_vwap(4, day, m);
    CHECK_FALSE(a.session_vwap_seed_needed(4, first_live, "1m").has_value());   // seeded once
    // The live window continues the same accumulation: price 120 on volume 4.
    ParsedAltTech w;
    w.kind = AltTechKind::bitget_microstructure;
    w.canonical_pair_id = 4;
    w.window_label = "1m";
    w.bar_open_time_ms = first_live;
    // No positional micro set configured here: exercise SessionVwap directly.
    SessionVwap sv;
    for (const auto& x : m) sv.record(x.open_ms, x.quote_volume / x.base_volume, x.base_volume);
    sv.record(first_live, 120.0, 4.0);
    REQUIRE(sv.value().has_value());
    CHECK(*sv.value() == Catch::Approx((300 * 100.0 + 300 * 330.0 + 480.0) / (300 + 900 + 4)));
    // A minute already counted by the seed is never counted twice.
    sv.record(day + 599 * 60'000LL, 999.0, 1000.0);
    CHECK(*sv.value() == Catch::Approx((300 * 100.0 + 300 * 330.0 + 480.0) / (300 + 900 + 4)));
}

TEST_CASE("the state between decision bars: last context, fresh estimates", "[assembler][state]") {
    AssemblerConfig cfg;
    cfg.bitget_features = test_set();
    cfg.max_forecast_age_ms = 180'000;
    ContextAssembler a(cfg);
    const int64_t now = 1'200'000;
    // No context built yet: no state to evaluate.
    CHECK_FALSE(a.state_snapshot(4, now).has_value());
    a.ingest_alttech(bitget_message(4, {1.0f, 2.0f, 3.0f, 4.0f, 5.0f}));
    a.ingest_alttech(regime_for(4));
    const auto fc = [&](int32_t h, int64_t bar, double net_long) {
        ParsedForecast f;
        f.canonical_pair_id = 4;
        f.view = altstrategy::engine::test::econ_forecast(h, net_long, -30.0, bar);
        a.ingest_forecast(f);
    };
    fc(60, now - 120'000, 1.0);
    const auto ctx = a.try_build(4, now);
    REQUIRE(ctx.has_value());
    // Two minutes later a newer estimate arrives, with no new decision bar.
    const int64_t later = now + 120'000;
    fc(60, later - 120'000, 7.0);
    const auto s = a.state_snapshot(4, later);
    REQUIRE(s.has_value());
    CHECK(s->context_id == ctx->context_id);                 // framed by the last context
    CHECK(s->forecasts.at(60).expected_net_long_bps == Catch::Approx(7.0));
    CHECK(s->event_time_ms == later - 60'000);               // the estimate's bar close
    // A context older than the state bound frames nothing.
    CHECK_FALSE(a.state_snapshot(4, now + cfg.state_max_context_age_ms + 1).has_value());
}
