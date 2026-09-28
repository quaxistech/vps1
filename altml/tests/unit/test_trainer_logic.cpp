// Contracts of the native trainer's pure logic: temporal splits with embargo,
// classification metrics, PG literal parsing and the economic protocol. These
// mirror the guarantees the retired Python test-suite pinned (tr-1.4.0).

#include "altml/components/trainer_logic.hpp"

#include <algorithm>
#include <limits>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace altml::training;
using Catch::Matchers::WithinAbs;

namespace {

constexpr int64_t kMinute = 60'000;

std::vector<int64_t> minute_grid(size_t n) {
    std::vector<int64_t> t(n);
    for (size_t i = 0; i < n; ++i) t[i] = static_cast<int64_t>(i) * kMinute;
    return t;
}

int64_t max_time(const std::vector<int64_t>& times, const std::vector<int64_t>& idx) {
    int64_t out = std::numeric_limits<int64_t>::min();
    for (const int64_t i : idx) out = std::max(out, times[static_cast<size_t>(i)]);
    return out;
}

int64_t min_time(const std::vector<int64_t>& times, const std::vector<int64_t>& idx) {
    int64_t out = std::numeric_limits<int64_t>::max();
    for (const int64_t i : idx) out = std::min(out, times[static_cast<size_t>(i)]);
    return out;
}

} // namespace

TEST_CASE("PG array and bytea literals parse with NULL and NaN semantics") {
    const auto vals = parse_pg_float_array("{1.5,-2,NULL,3e-4}");
    REQUIRE(vals.size() == 4);
    CHECK_THAT(vals[0], WithinAbs(1.5, 1e-9));
    CHECK_THAT(vals[1], WithinAbs(-2.0, 1e-9));
    CHECK(std::isnan(vals[2]));
    CHECK_THAT(vals[3], WithinAbs(3e-4, 1e-8)); // float32 representation error
    CHECK(parse_pg_float_array("{}").empty());
    CHECK_THROWS(parse_pg_float_array("1,2,3"));

    const auto bytes = parse_pg_hex_bytea("\\x0aff");
    REQUIRE(bytes.size() == 2);
    CHECK(bytes[0] == 0x0a);
    CHECK(bytes[1] == 0xff);
    CHECK(bitmap_bit({0b00000101}, 0));
    CHECK_FALSE(bitmap_bit({0b00000101}, 1));
    CHECK(bitmap_bit({0b00000101}, 2));
    CHECK_FALSE(bitmap_bit({0b00000101}, 8)); // out of range -> invalid
    CHECK_THROWS(parse_pg_hex_bytea("0aff"));
    CHECK_THROWS(parse_pg_hex_bytea("\\x0af"));
}

TEST_CASE("walk-forward folds are expanding, ordered and embargoed") {
    const auto times = minute_grid(800);
    const int64_t embargo = 30 * kMinute;
    const auto folds = build_walk_forward_folds(times, 4, embargo);
    REQUIRE(folds.size() == 4);
    for (size_t k = 0; k < folds.size(); ++k) {
        const auto& fold = folds[k];
        REQUIRE_FALSE(fold.train.empty());
        REQUIRE_FALSE(fold.valid.empty());
        REQUIRE_FALSE(fold.test.empty());
        // train strictly before valid with a full embargo gap; valid before test.
        CHECK(max_time(times, fold.train) < min_time(times, fold.valid) - embargo + kMinute);
        CHECK(max_time(times, fold.valid) < min_time(times, fold.test));
        if (k > 0) {
            // expanding: each fold trains on at least everything the previous saw
            CHECK(fold.train.size() >= folds[k - 1].train.size());
        }
    }
    CHECK_THROWS(build_walk_forward_folds(minute_grid(6), 4, embargo));
}

TEST_CASE("final split reserves a test slice untouched by fold tests") {
    const auto times = minute_grid(800);
    const int64_t embargo = 30 * kMinute;
    const auto folds = build_walk_forward_folds(times, 4, embargo);
    const auto final_split = build_final_split(times, 4, embargo, 0.15);
    const int64_t final_test_start = min_time(times, final_split.test);
    for (const auto& fold : folds) {
        CHECK(max_time(times, fold.test) < final_test_start);
    }
    // refit rows never reach the embargoed zone before the final test
    CHECK(max_time(times, final_split.fit) < final_test_start - embargo + kMinute);
    // validation is prior, non-test data
    CHECK(max_time(times, final_split.valid) < final_test_start);
}

TEST_CASE("roc_auc and log_loss match hand-computed values") {
    // p ranks: perfect separation -> AUC 1; anti-separation -> 0.
    CHECK_THAT(roc_auc({0, 0, 1, 1}, {0.1, 0.2, 0.8, 0.9}), WithinAbs(1.0, 1e-12));
    CHECK_THAT(roc_auc({1, 1, 0, 0}, {0.1, 0.2, 0.8, 0.9}), WithinAbs(0.0, 1e-12));
    // ties get average rank: y={0,1} with equal p -> AUC 0.5.
    CHECK_THAT(roc_auc({0, 1}, {0.5, 0.5}), WithinAbs(0.5, 1e-12));
    CHECK(std::isnan(roc_auc({1, 1}, {0.4, 0.6}))); // single class undefined
    const double ll = log_loss({1, 0}, {0.8, 0.2});
    CHECK_THAT(ll, WithinAbs(-std::log(0.8), 1e-12));
}

TEST_CASE("conviction threshold is a prior-slice quantile and validates input") {
    // |p-0.5| = {0.0, 0.1, 0.2, 0.3, 0.4}; quantile 0.5 -> 0.2
    CHECK_THAT(conviction_threshold({0.5, 0.6, 0.7, 0.8, 0.9}, 0.5), WithinAbs(0.2, 1e-12));
    CHECK_THAT(conviction_threshold({0.5, 0.6}, 1.0), WithinAbs(0.1, 1e-12));
    CHECK_THROWS(conviction_threshold({}, 0.8));
    CHECK_THROWS(conviction_threshold({1.5}, 0.8));  // outside [0,1]
    CHECK_THROWS(conviction_threshold({0.5}, 1.5));  // bad quantile
}

TEST_CASE("trade selection is one position per window with deterministic ties") {
    // Three symbols at t=0; symbol 1 has the strongest conviction and must win.
    const std::vector<double> p = {0.6, 0.9, 0.7, 0.95, 0.9};
    const std::vector<int64_t> t = {0, 0, 0, 10 * kMinute, 20 * kMinute};
    const std::vector<int32_t> sym = {0, 1, 2, 0, 1};
    const auto selected = select_non_overlapping(p, t, sym, 0.05, 15 * kMinute);
    REQUIRE(selected.size() == 2);
    CHECK(selected[0] == 1);          // best conviction at t=0
    CHECK(selected[1] == 4);          // t=10m blocked by 15m holding, t=20m free
    // ties on conviction break by symbol id then row index
    const auto tie = select_non_overlapping({0.9, 0.9}, {0, 0}, {1, 0}, 0.0, kMinute);
    REQUIRE(tie.size() == 1);
    CHECK(tie[0] == 1); // symbol 0 wins the tie
    CHECK_THROWS(select_non_overlapping({0.6}, {0, 1}, {0, 0}, 0.1, kMinute));
    CHECK_THROWS(select_non_overlapping({0.6}, {0}, {0}, 0.7, kMinute));
}

TEST_CASE("directional economics use linear payoff and explicit costs") {
    // One long trade: raw log return ln(1.01); fees 12bps + slippage 4bps.
    const std::vector<double> p = {0.9};
    const std::vector<double> r = {std::log(1.01)};
    const auto econ = evaluate_directional({0}, p, r, 12.0, 4.0);
    CHECK(econ.trades == 1);
    CHECK_THAT(econ.expectancy_after_costs, WithinAbs(0.01 - 0.0016, 1e-12));
    CHECK_THAT(econ.long_share, WithinAbs(1.0, 1e-12));
    // Short side: p<0.5 profits from a fall on a linear contract.
    const auto short_econ = evaluate_directional({0}, {0.1}, {std::log(0.99)}, 12.0, 4.0);
    CHECK_THAT(short_econ.expectancy_after_costs, WithinAbs(0.01 - 0.0016, 1e-12));
    // A flat market row still pays full costs.
    const auto flat = evaluate_directional({0}, {0.9}, {0.0}, 12.0, 4.0);
    CHECK_THAT(flat.expectancy_after_costs, WithinAbs(-0.0016, 1e-12));
    // Drawdown is multiplicative and bounded by the losing streak.
    const auto dd = evaluate_directional({0, 1}, {0.9, 0.9},
                                         {std::log(0.9), std::log(0.9)}, 0.0, 0.0);
    CHECK(dd.max_drawdown > 0.18);
    CHECK(dd.max_drawdown < 0.20);
    CHECK(dd.profit_factor == 0.0);
}

// Regression: the sampling loop coarsened against max_rows while the gate after
// it refused against max_matrix_mib. A dataset could clear the loop and still be
// rejected, with nothing left to try — the trainer had not completed a cycle
// since 2026-08-09 while ingesting roughly 955,000 feature vectors.
TEST_CASE("the training row cap honours both the row and the memory ceiling",
          "[trainer][regression]") {
    using namespace altml::training;

    SECTION("memory cap is derived from features and budget") {
        // 1024 MiB against 300 features: 1024*1024*1024 / (16*300).
        REQUIRE(memory_row_cap(1024, 300) == (1024LL * 1024 * 1024) / (16 * 300));
        REQUIRE(memory_row_cap(1024, 300) > 0);
    }

    SECTION("the binding ceiling is the smaller of the two") {
        // Memory is the tighter bound: a generous row policy must not win.
        REQUIRE(training_row_cap(10'000'000, 1024, 300) == memory_row_cap(1024, 300));
        // ...and a strict row policy must not be overridden by a large budget.
        REQUIRE(training_row_cap(50'000, 100'000, 300) == 50'000);
    }

    SECTION("the two ceilings can no longer disagree") {
        // The defect: a candidate that satisfies max_rows but not the memory
        // bound. Under one combined cap this cannot arise.
        constexpr int64_t kMaxRows = 10'000'000;
        constexpr int64_t kBudgetMib = 1024;
        constexpr int32_t kFeatures = 300;
        const int64_t cap = training_row_cap(kMaxRows, kBudgetMib, kFeatures);

        const int64_t matrix_bytes = cap * kFeatures * 4;
        REQUIRE(matrix_bytes * 4 <= kBudgetMib * 1024 * 1024);   // the gate passes
        REQUIRE(cap <= kMaxRows);                                 // and so does the policy
    }

    SECTION("a bigger host automatically trains on more rows") {
        // The same code must use an expanded budget without being retuned.
        const int64_t small = training_row_cap(10'000'000, 1024, 300);
        const int64_t large = training_row_cap(10'000'000, 8192, 300);
        REQUIRE(large > small);
        // Eight times the budget buys eight times the rows. Integer division
        // truncates, so the relationship is asserted to within one row rather
        // than exactly — the property that matters is proportionality, not the
        // remainder.
        REQUIRE(large - small * 8 >= 0);
        REQUIRE(large - small * 8 <= 8);
    }

    SECTION("degenerate inputs never yield a zero or negative cap") {
        REQUIRE(memory_row_cap(1024, 0) == 0);
        REQUIRE(memory_row_cap(0, 300) == 0);
        REQUIRE(training_row_cap(100, 0, 300) == 100);   // falls back to the row policy
        REQUIRE(training_row_cap(0, 1024, 300) >= 1);
    }
}

// --------------------------------------------------- T0 alignment

TEST_CASE("an aligned source may only use a bar that has already closed") {
    using altml::training::AlignedSource;
    AlignedSource h1 = *altml::training::parse_aligned_source("2:1h:0", 1);
    h1.equivalent_versions = {1};
    const std::string sql = altml::training::aligned_source_sql(h1);

    // The whole point: the comparison is on the source bar's CLOSE against the
    // target's close, not on the two open times. A 1h bar that opened at the
    // same minute as the target has not finished forming, and joining it would
    // hand the model the answer dressed as a feature.
    //
    // The bound is expressed with the span moved to the TARGET side, which is
    // the same set of rows (`a + i <= b` iff `a <= b - i`) and the only form an
    // index can use. Written the other way the planner has to compute the
    // column before comparing it, so the bound fell into a Filter and every
    // LATERAL scanned the whole instrument: measured on VPS1, 5493 ms against
    // 37 ms for one source over 24 hours. With six sources the training query
    // hit the statement timeout and no model was ever produced.
    CHECK(sql.find("s1_i.bar_open_time <= f.bar_open_time + interval '1 minute'"
                   " - interval '60 minutes'") != std::string::npos);
    // Sargability is the property under test, so assert the shape directly:
    // the indexed column must never appear inside an arithmetic expression.
    CHECK(sql.find("s1_i.bar_open_time + interval") == std::string::npos);
    CHECK(sql.find("ORDER BY s1_i.bar_open_time DESC LIMIT 1") != std::string::npos);
    // LEFT JOIN: a missing observation stays missing rather than dropping the row.
    CHECK(sql.rfind("LEFT JOIN LATERAL", 0) == 0);
    // Venue and grid are pinned; nothing may substitute another venue's bar.
    CHECK(sql.find("exchange_id = 2") != std::string::npos);
    CHECK(sql.find("timeframe = '1h'") != std::string::npos);
}

TEST_CASE("freshness bounds how stale an aligned observation may be") {
    using altml::training::AlignedSource;
    const std::string none =
        altml::training::aligned_source_sql(*altml::training::parse_aligned_source("3:5m:0", 2));
    CHECK(none.find(">=") == std::string::npos);   // no lower bound requested

    const std::string bounded =
        altml::training::aligned_source_sql(*altml::training::parse_aligned_source("3:5m:30", 2));
    CHECK(bounded.find("- interval '30 minutes'") != std::string::npos);
    // The lower bound is sargable too: span and freshness are both subtracted
    // from the target side rather than added to the indexed column.
    CHECK(bounded.find("s2_i.bar_open_time >= f.bar_open_time + interval '1 minute'"
                       " - interval '5 minutes' - interval '30 minutes'") != std::string::npos);
    CHECK(bounded.find("s2_i.bar_open_time + interval") == std::string::npos);
}

TEST_CASE("the served input has exactly the width the booster was trained on") {
    using altml::training::aligned_feature_names;
    using altml::training::parse_aligned_source;

    // The defect this pins down: the trainer appended an aligned block to the
    // matrix and registered the widened count, while inference fed the booster
    // the published vector alone. The booster was trained on 1067 columns and
    // offered 995, so every bar was refused as `model_schema_mismatch` and a
    // successfully trained, promoted model produced no predictions at all --
    // silently, because a rejected vector looks the same as a quiet market.
    const std::vector<std::string> specs = {"2:1m:5", "3:1m:5", "1:1h:180",
                                            "1:1m:5:reference_lite:4"};
    const std::vector<std::string> core_layout = {"RSI_14_outReal", "ATR_14_outReal",
                                                  "OBV_outReal", "SMA_5_outReal"};
    const std::vector<std::string> lite_layout = {"spread", "book_imbalance", "cvd"};
    const std::vector<std::string> core_projection = {"RSI_14_outReal", "OBV_outReal"};

    std::vector<altml::training::AlignedSource> sources;
    for (size_t i = 0; i < specs.size(); ++i) {
        auto src = parse_aligned_source(specs[i], i);
        REQUIRE(src.has_value());
        const auto& layout = src->feature_set == "talib-core" ? core_layout : lite_layout;
        REQUIRE(altml::training::resolve_source_projection(*src, layout, core_projection,
                                                           "talib-core"));
        sources.push_back(*src);
    }

    const int base_feature_count = 995;
    const auto names = aligned_feature_names(sources);
    // talib-core sources contribute the projection; reference_lite is taken
    // whole, so sources no longer contribute equally.
    CHECK(names.size() == 3 * core_projection.size() + lite_layout.size());
    CHECK(altml::training::aligned_block_width(sources) == names.size());

    // Trainer side: matrix width. Inference side: base vector plus the same
    // block. The two must be the same number.
    const size_t trained_width = static_cast<size_t>(base_feature_count) + names.size();
    size_t served_width = static_cast<size_t>(base_feature_count);
    for (const auto& src : sources) served_width += src.positions.size();
    CHECK(trained_width == served_width);
    CHECK(trained_width == 995 + 6 + 3);

    // Order is part of the contract: source-major, then projection, with venue,
    // grid and set readable in the name. A block assembled in a different order
    // would still have the right width and would score noise.
    CHECK(names[0] == "v2_1m_talib-core_RSI_14_outReal");
    CHECK(names[1] == "v2_1m_talib-core_OBV_outReal");
    CHECK(names[2] == "v3_1m_talib-core_RSI_14_outReal");
    CHECK(names[6] == "v1_1m_reference_lite_spread");
    CHECK(names.back() == "v1_1m_reference_lite_cvd");

    // Positions follow the layout of the set the source actually reads.
    CHECK(sources[0].positions == std::vector<size_t>{0, 2});
    CHECK(sources[3].positions == std::vector<size_t>{0, 1, 2});
}

TEST_CASE("a source names the feature set it reads, and its version") {
    using altml::training::aligned_source_spec;
    using altml::training::aligned_source_sql;
    using altml::training::parse_aligned_source;

    // The gap this closes: aligned_source_sql() had the set name hardcoded to
    // "talib-core", so reference_lite -- published by AltTech for every venue
    // and archived by AltML all along -- could not reach the trainer at all.
    // The execution venue's own spread, book depth, imbalance, flow, funding
    // and open interest were sitting in the archive, unread.
    const auto lite = parse_aligned_source("1:1m:5:reference_lite:4", 0);
    REQUIRE(lite.has_value());
    CHECK(lite->feature_set == "reference_lite");
    CHECK(lite->feature_set_version == 4);
    auto resolved = *lite;
    // reference_lite_micro publishes four frozen versions with a byte-identical
    // column list, so all four select the same layout.
    resolved.equivalent_versions = {1, 2, 3, 4};
    const std::string sql = aligned_source_sql(resolved);
    CHECK(sql.find("feature_set_name = 'reference_lite'") != std::string::npos);
    // Rows are selected by LAYOUT, not by version number. Both matter and they
    // are not the same thing: reference_lite v3 and v4 share a name and differ
    // in columns, so the name alone would blend two layouts into one column --
    // but reference_lite_micro publishes four frozen versions with a byte-
    // identical column list, so a version match would have hidden 374k rows of
    // history that the layout match finds.
    CHECK(sql.find("feature_set_version = ANY(ARRAY[1,2,3,4])") != std::string::npos);
    // schema_hash is NOT used: AltTech leaves it empty on the wire for
    // reference_lite and cross_venue_core, so those rows carry none.
    CHECK(sql.find("schema_hash") == std::string::npos);

    // The short form still means talib-core, which is what every source was.
    const auto core = parse_aligned_source("2:1m:5", 1);
    REQUIRE(core.has_value());
    CHECK(core->feature_set == "talib-core");
    CHECK(core->feature_set_version == 1);
    auto core_resolved = *core;
    core_resolved.equivalent_versions = {1};
    CHECK(aligned_source_sql(core_resolved).find("feature_set_name = 'talib-core'") !=
          std::string::npos);

    // A spec round-trips through its stored form, so a model records the
    // sources it was built from rather than whatever the config says later.
    CHECK(aligned_source_spec(*lite) == "1:1m:5:reference_lite:4");
    CHECK(parse_aligned_source(aligned_source_spec(*core), 1)->feature_set == "talib-core");
    CHECK_FALSE(parse_aligned_source("1:1m:5:reference_lite:0", 0).has_value());
    CHECK_FALSE(parse_aligned_source("1:1m:5:reference_lite", 0).has_value());
}

TEST_CASE("venue 0 means the consensus, and only the consensus set") {
    using altml::training::aligned_source_sql;
    using altml::training::parse_aligned_source;

    const auto x = parse_aligned_source("0:1m:5:cross_venue_core:2", 0);
    REQUIRE(x.has_value());
    CHECK(x->exchange_id == 0);
    CHECK(x->feature_set == "cross_venue_core");
    // It joins on the same instrument key every venue uses: the archive stores
    // the canonical pair id as instrument_id for venue vectors too, so no
    // translation is needed and none is invented.
    const std::string sql = aligned_source_sql(*x);
    CHECK(sql.find("exchange_id = 0") != std::string::npos);
    CHECK(sql.find("s0_i.instrument_id = f.instrument_id") != std::string::npos);

    // A per-venue set has no consensus edition and vice versa. Either mix-up
    // would join a set that does not exist and produce a column of NULLs that
    // reads as a quiet market rather than as an error.
    CHECK_FALSE(parse_aligned_source("0:1m:5", 0).has_value());
    CHECK_FALSE(parse_aligned_source("0:1m:5:reference_lite:4", 0).has_value());
    CHECK_FALSE(parse_aligned_source("1:1m:5:cross_venue_core:2", 0).has_value());
    CHECK_FALSE(parse_aligned_source("-1:1m:5", 0).has_value());
}

TEST_CASE("the macro block is pinned to instrument 0 in training and in serving") {
    using altml::training::aligned_block_width;
    using altml::training::aligned_feature_names;
    using altml::training::aligned_source_spec;
    using altml::training::aligned_source_sql;
    using altml::training::parse_aligned_source;

    // The sixth field pins the join. Without it the LATERAL would look for
    // macro rows under each target's own instrument id, find none, and the
    // whole block would train as NaN -- and serve as NaN -- while looking
    // configured.
    auto src = parse_aligned_source("0:1m:5:macro-core:1:0", 0);
    REQUIRE(src.has_value());
    CHECK(src->exchange_id == 0);
    CHECK(src->feature_set == "macro-core");
    REQUIRE(src->pinned_instrument_id.has_value());
    CHECK(*src->pinned_instrument_id == 0);
    const std::string sql = aligned_source_sql(*src);
    CHECK(sql.find("s0_i.instrument_id = 0 ") != std::string::npos);
    CHECK(sql.find("f.instrument_id") == std::string::npos);
    // The recipe stored with the model carries the pin, so the inference side
    // reconstructs the same join -- the served vector must be built from the
    // same observation the matrix was.
    CHECK(aligned_source_spec(*src) == "0:1m:5:macro-core:1:0");
    // A pin on a venue vector is meaningless and refused.
    CHECK_FALSE(parse_aligned_source("1:1m:5:reference_lite:4:0", 0).has_value());

    // Width: the block is taken whole, and its 40 names carry the pin's venue
    // and grid like any other source's.
    std::vector<std::string> layout;
    for (int i = 0; i < 40; ++i) layout.push_back("m" + std::to_string(i));
    REQUIRE(altml::training::resolve_source_projection(*src, layout, {}, "talib-core"));
    std::vector<altml::training::AlignedSource> sources{*src};
    CHECK(aligned_block_width(sources) == 40);
    const auto names = aligned_feature_names(sources);
    REQUIRE(names.size() == 40);
    CHECK(names[0] == "v0_1m_macro-core_m0");
    CHECK(names[39] == "v0_1m_macro-core_m39");
}

TEST_CASE("a projection name absent from its set is refused, not skipped") {
    using altml::training::parse_aligned_source;
    using altml::training::resolve_source_projection;
    auto src = *parse_aligned_source("2:1m:5", 0);
    const std::vector<std::string> layout = {"RSI_14_outReal", "ATR_14_outReal"};
    CHECK(resolve_source_projection(src, layout, {"RSI_14_outReal"}, "talib-core"));
    // Skipping an unknown name would shrink the feature space without saying so.
    CHECK_FALSE(resolve_source_projection(src, layout, {"RSI_14_outReal", "NOPE"}, "talib-core"));
    CHECK_FALSE(resolve_source_projection(src, {}, {"RSI_14_outReal"}, "talib-core"));
}

TEST_CASE("a malformed or out-of-range source spec is refused, not defaulted") {
    using altml::training::parse_aligned_source;
    CHECK(parse_aligned_source("2:1m:5", 0).has_value());
    CHECK_FALSE(parse_aligned_source("2:1m", 0).has_value());       // no freshness
    CHECK_FALSE(parse_aligned_source("2:7m:5", 0).has_value());     // unknown grid
    CHECK_FALSE(parse_aligned_source("0:1m:5", 0).has_value());     // venue out of range
    CHECK_FALSE(parse_aligned_source("5:1m:5", 0).has_value());     // venue out of range
    CHECK_FALSE(parse_aligned_source("2:1m:-1", 0).has_value());    // negative freshness
    CHECK(parse_aligned_source("2:1m:5", 3)->alias == "s3");
}

TEST_CASE("in-memory T0 admission matches the SQL rule the trainer joins with") {
    using altml::training::aligned_observation_usable;
    using altml::training::parse_aligned_source;
    constexpr int64_t kMin = 60'000;

    // Target 1m bar opening at T, so T0 -- the instant a decision could be made
    // -- is T + 1m.
    const int64_t target_open = 1'700'000'000'000 / kMin * kMin;
    const int64_t t0 = target_open + kMin;

    const auto h1 = *parse_aligned_source("1:1h:180", 0);
    // A 1h bar that opened at the same minute closes 59 minutes after T0: it
    // has not finished forming, and admitting it is the expensive leak.
    CHECK_FALSE(aligned_observation_usable(h1, target_open, t0));
    // One that closes exactly at T0 is admissible -- the boundary is inclusive,
    // matching `<=` in the SQL.
    CHECK(aligned_observation_usable(h1, t0 - 60 * kMin, t0));
    // Within the 180-minute freshness bound, still admissible.
    CHECK(aligned_observation_usable(h1, t0 - 200 * kMin, t0));
    // Past it, absent -- and absent must stay absent rather than become stale.
    CHECK_FALSE(aligned_observation_usable(h1, t0 - 260 * kMin, t0));

    // freshness 0 is UNBOUNDED below, not "exactly at T0": the SQL emits a
    // lower bound only for a positive freshness, so the newest closed bar wins
    // however old it is. The declaration said the opposite; the two sides of
    // the join agreed with each other and only the comment was wrong, which is
    // the kind of disagreement that survives review because nothing fails.
    const auto unbounded = *parse_aligned_source("2:1m:0", 1);
    CHECK(aligned_observation_usable(unbounded, t0 - kMin, t0));
    CHECK(aligned_observation_usable(unbounded, t0 - 2 * kMin, t0));
    CHECK(aligned_observation_usable(unbounded, t0 - 5000 * kMin, t0));
    // A bar that has not closed by T0 is still refused -- that bound is not
    // conditional on freshness.
    CHECK_FALSE(aligned_observation_usable(unbounded, t0, t0));
}

TEST_CASE("a text array survives the round trip that carries the input recipe") {
    using altml::training::parse_pg_text_array;
    using altml::training::pg_text_array_literal;
    const std::vector<std::string> specs = {"2:1m:5", "1:1h:180"};
    CHECK(parse_pg_text_array(pg_text_array_literal(specs)) == specs);
    // Arity is what matters: an element containing a separator must not split.
    const std::vector<std::string> awkward = {"a,b", "c\"d", "e\\f", ""};
    CHECK(parse_pg_text_array(pg_text_array_literal(awkward)) == awkward);
    CHECK(parse_pg_text_array("{}").empty());
}

TEST_CASE("every supported grid has a span, and an unknown grid is refused") {
    using altml::training::grid_span_minutes;
    CHECK(grid_span_minutes("1m").value() == 1);
    CHECK(grid_span_minutes("5m").value() == 5);
    CHECK(grid_span_minutes("15m").value() == 15);
    CHECK(grid_span_minutes("30m").value() == 30);
    CHECK(grid_span_minutes("1h").value() == 60);
    CHECK(grid_span_minutes("4h").value() == 240);
    CHECK(grid_span_minutes("1d").value() == 1440);
    // Defaulting an unknown grid to 1m would shift the alignment silently.
    CHECK_FALSE(grid_span_minutes("2h").has_value());
    CHECK_FALSE(grid_span_minutes("").has_value());
}

// ------------------------------------------- fold-local feature selection

TEST_CASE("selection uses only the rows it is given") {
    using namespace altml::training;
    // 4 rows x 3 features. Feature 2 is constant on rows {0,1} but varies once
    // row 3 is included -- so the verdict MUST differ with the row set. If it
    // does not, the selector is looking outside its fold.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const std::vector<float> X = {
        1.0F, nan, 5.0F,
        2.0F, nan, 5.0F,
        3.0F, nan, 5.0F,
        4.0F, nan, 9.0F,
    };
    const auto early = select_features_fold_local(X, 3, {0, 1, 2});
    CHECK(early.reason[2] == RejectReason::constant);
    CHECK(std::find(early.selected.begin(), early.selected.end(), 2u) == early.selected.end());

    const auto late = select_features_fold_local(X, 3, {0, 1, 2, 3});
    CHECK(late.reason[2] == RejectReason::kept);
    CHECK(std::find(late.selected.begin(), late.selected.end(), 2u) != late.selected.end());

    // Feature 1 is missing everywhere, in both row sets.
    CHECK(early.reason[1] == RejectReason::all_missing);
    CHECK(late.reason[1] == RejectReason::all_missing);
}

TEST_CASE("a feature missing on most rows is rejected, and the reason is recorded") {
    using namespace altml::training;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    // Feature 1 present on 1 of 5 rows -> 80% missing, above the 60% bar.
    const std::vector<float> X = {
        1.0F, 7.0F,
        2.0F, nan,
        3.0F, nan,
        4.0F, nan,
        5.0F, nan,
    };
    const auto sel = select_features_fold_local(X, 2, {0, 1, 2, 3, 4});
    CHECK(sel.reason[1] == RejectReason::extreme_missingness);
    CHECK(sel.selected.size() == 1);
    CHECK(sel.selected[0] == 0u);
    CHECK(sel.rejected() == 1);
    CHECK(std::string(reject_reason_name(sel.reason[1])) == "extreme_missingness");
}

TEST_CASE("identical columns are collapsed, keeping the earlier one") {
    using namespace altml::training;
    const std::vector<float> X = {
        1.0F, 1.0F, 3.0F,
        2.0F, 2.0F, 9.0F,
        3.0F, 3.0F, 4.0F,
    };
    const auto sel = select_features_fold_local(X, 3, {0, 1, 2});
    CHECK(sel.reason[0] == RejectReason::kept);
    CHECK(sel.reason[1] == RejectReason::duplicate);
    CHECK(sel.reason[2] == RejectReason::kept);
    CHECK(sel.selected.size() == 2);
}

TEST_CASE("an empty fold selects nothing rather than everything") {
    using namespace altml::training;
    const std::vector<float> X = {1.0F, 2.0F};
    const auto sel = select_features_fold_local(X, 2, {});
    CHECK(sel.selected.empty());
}

// ------------------------------------------------- cost model provenance

// Round-trip cost has one canonical home: altml.fee_models, the same row the
// label builder reads for `future_return_after_fees_*`. The trainer used to
// carry its own literals (12.0 / 4.0 bps) with no link to that table, so the
// labels and the economic gate could price a trade differently and nothing
// would report the disagreement.
TEST_CASE("round-trip cost is derived from the fee model, both legs charged") {
    using altml::training::round_trip_from_fee_model;

    SECTION("the deployed fee model reproduces the values it replaced") {
        // altml.fee_models v1: taker_fee 0.0006, slippage_estimate_bps 2.0.
        const auto c = round_trip_from_fee_model(0.0006, 2.0);
        CHECK(c.fee_bps == Catch::Approx(12.0));        // 6 bps in, 6 bps out
        CHECK(c.slippage_bps == Catch::Approx(4.0));    // crossed twice
    }

    SECTION("a changed fee model moves the cost with it") {
        const auto cheaper = round_trip_from_fee_model(0.0002, 1.0);
        CHECK(cheaper.fee_bps == Catch::Approx(4.0));
        CHECK(cheaper.slippage_bps == Catch::Approx(2.0));
        const auto dearer = round_trip_from_fee_model(0.0010, 3.5);
        CHECK(dearer.fee_bps == Catch::Approx(20.0));
        CHECK(dearer.slippage_bps == Catch::Approx(7.0));
    }

    SECTION("a zero-cost model is representable and not special-cased") {
        const auto free = round_trip_from_fee_model(0.0, 0.0);
        CHECK(free.fee_bps == Catch::Approx(0.0));
        CHECK(free.slippage_bps == Catch::Approx(0.0));
    }
}
