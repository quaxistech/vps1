// The econ feature space: projection, derived geometry, naming, point-in-time SQL.

#include "altml/components/feature_recipe.hpp"
#include "altml/components/trainer_logic.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <map>

using namespace altml;
using Catch::Approx;

namespace {

// A layout containing every derived input, in an arbitrary order, plus noise.
std::vector<std::string> layout() {
    std::vector<std::string> l = {"NOISE_A"};
    for (const auto* n : recipe::DerivedRecipe::input_names()) l.emplace_back(n);
    l.emplace_back("NOISE_B");
    return l;
}

std::vector<float> vector_for(const std::vector<std::string>& l,
                              const std::map<std::string, float>& v) {
    std::vector<float> out(l.size(), std::numeric_limits<float>::quiet_NaN());
    for (size_t i = 0; i < l.size(); ++i) {
        if (auto it = v.find(l[i]); it != v.end()) out[i] = it->second;
    }
    return out;
}

} // namespace

TEST_CASE("derived geometry is in the instrument's own volatility units", "[recipe]") {
    const auto l = layout();
    const auto r = recipe::DerivedRecipe::resolve(l);
    REQUIRE(r.ok);
    // close = 2*WCL - MED = 2*101 - 100 = 102.
    std::map<std::string, float> v = {
        {"WCLPRICE_outReal", 101}, {"MEDPRICE_outReal", 100}, {"EMA_20_outReal", 100},
        {"EMA_50_outReal", 98}, {"EMA_200_outReal", 95}, {"ATR_14_outReal", 2},
        {"MAX_HIGH_20_outReal", 104}, {"MIN_LOW_20_outReal", 100}, {"MAX_HIGH_50_outReal", 110},
        {"MIN_LOW_50_outReal", 90}, {"MAX_HIGH_200_outReal", 120}, {"MIN_LOW_200_outReal", 80},
        {"NATR_14_outReal", 2}, {"NATR_200_outReal", 1}, {"PLUS_DI_14_outReal", 30},
        {"MINUS_DI_14_outReal", 10}, {"MACD_outMACDHist", 1}, {"SMA_VOL_5_outReal", 300},
        {"SMA_VOL_50_outReal", 100}, {"SMA_VOL_200_outReal", 50}, {"STDDEV_5_outReal", 1},
        {"LINEARREG_SLOPE_14_outReal", 0.5}, {"LINEARREG_SLOPE_50_outReal", 0.2}};
    const auto full = vector_for(l, v);
    std::vector<float> out(recipe::DerivedRecipe::width());
    r.compute(full.data(), full.size(), out.data());
    const auto& n = recipe::derived_names();
    const auto at = [&](const char* name) {
        return out[static_cast<size_t>(std::find(n.begin(), n.end(), name) - n.begin())];
    };
    CHECK(at("dist_ema20_atr") == Approx(1.0));     // (102 - 100) / 2
    CHECK(at("dist_ema200_atr") == Approx(3.5));
    CHECK(at("don_pos_20") == Approx(0.5));         // (102 - 100) / (104 - 100)
    CHECK(at("natr_ratio_14_200") == Approx(2.0));
    CHECK(at("di_diff_14") == Approx(20.0));
    CHECK(at("vol_ratio_5_50") == Approx(3.0));
    CHECK(at("ema_stack") == Approx(1.0));          // 102 > 100 > 98 > 95
}

TEST_CASE("a missing input is a missing output, never zero", "[recipe][missing]") {
    const auto l = layout();
    const auto r = recipe::DerivedRecipe::resolve(l);
    REQUIRE(r.ok);
    // No ATR, no EMAs: every ATR-normalised and stack output is NaN.
    const auto full = vector_for(l, {{"WCLPRICE_outReal", 101}, {"MEDPRICE_outReal", 100}});
    std::vector<float> out(recipe::DerivedRecipe::width(), 0.0f);
    r.compute(full.data(), full.size(), out.data());
    for (const float x : out) CHECK(std::isnan(x));
    // A zero-width channel is not "position 0.5" or "position 0": it is unknown.
    auto flat = vector_for(l, {{"WCLPRICE_outReal", 101}, {"MEDPRICE_outReal", 100},
                               {"MAX_HIGH_20_outReal", 100}, {"MIN_LOW_20_outReal", 100}});
    r.compute(flat.data(), flat.size(), out.data());
    CHECK(std::isnan(out[3]));
}

TEST_CASE("an unresolvable layout is refused, not narrowed", "[recipe]") {
    auto l = layout();
    l.erase(std::find(l.begin(), l.end(), "ATR_14_outReal"));
    CHECK_FALSE(recipe::DerivedRecipe::resolve(l).ok);
}

TEST_CASE("the projection holds no price levels", "[recipe][scale]") {
    for (const auto& n : recipe::talib_projection()) {
        for (const char* level : {"SMA_", "EMA_", "KAMA_", "BBANDS", "MIDPOINT", "MIDPRICE",
                                  "TYPPRICE", "OBV", "LINEARREG_", "MAX_", "MIN_"}) {
            CHECK(n.rfind(level, 0) != 0);
        }
    }
    CHECK(recipe::is_excluded("cross_venue_core", "consensus_mid_price"));
    CHECK(recipe::is_excluded("macro-core", "equity_age_hours"));
    CHECK_FALSE(recipe::is_excluded("cross_venue_core", "bitget_mid_deviation_bps"));
}

TEST_CASE("names are identical however they are rebuilt", "[recipe][naming]") {
    std::vector<std::string> a, b;
    recipe::append_base_names({"RSI_14_outReal"}, a);
    recipe::append_source_names(recipe::source_prefix(1, "15m", "talib-core"), {"RSI_14_outReal"},
                                true, a);
    recipe::append_base_names({"RSI_14_outReal"}, b);
    recipe::append_source_names("v1_15m_talib-core_", {"RSI_14_outReal"}, true, b);
    CHECK(recipe::joined_names(a) == recipe::joined_names(b));
    CHECK(a[1] == "b_d_dist_ema20_atr");
    CHECK(a[1 + recipe::derived_names().size()] == "v1_15m_talib-core_RSI_14_outReal");
}

TEST_CASE("the training join admits only what had been computed by T0", "[recipe][pit][leakage]") {
    auto src = training::parse_aligned_source("1:15m:60", 3);
    REQUIRE(src.has_value());
    src->equivalent_versions = {1};
    const auto sql = training::aligned_source_sql_pit(*src);
    // Closed by T0 AND computed no later than the target's own vector.
    CHECK(sql.find("s3_i.bar_open_time <= f.bar_open_time + interval '1 minute' - interval '15 minutes'") !=
          std::string::npos);
    CHECK(sql.find("s3_i.computed_at <= f.computed_at") != std::string::npos);
    CHECK(sql.find("s3_i.received_at <= f.received_at") != std::string::npos);
    CHECK(sql.find("altml.econ_feature_vectors") != std::string::npos);
    // The bitmap travels with the values, so an invalid slot can become NaN.
    CHECK(sql.find("validity_bitmap AS bmp") != std::string::npos);
    CHECK(sql.find("quality_status = 'VALID'") != std::string::npos);
}

TEST_CASE("aligned dependencies obey independent computation and receipt cutoffs", "[recipe][pit]") {
    const auto src = training::parse_aligned_source("1:1m:5:bitget_micro:1", 0);
    REQUIRE(src);
    constexpr int64_t t0 = 600'000;
    const auto usable = [&](int64_t computed, int64_t received) {
        return training::aligned_observation_usable_pit(*src, 540'000, computed, received,
                                                        t0, 601'000, 602'000);
    };
    CHECK(usable(600'500, 601'500));
    CHECK(usable(601'000, 602'000));
    CHECK_FALSE(usable(601'001, 601'500));
    CHECK_FALSE(usable(600'500, 602'001));
    CHECK_FALSE(usable(0, 601'500));
    CHECK_FALSE(usable(600'500, 0));
    CHECK_FALSE(training::aligned_observation_usable_pit(*src, 600'000,
        600'500, 601'500, t0, 601'000, 602'000)); // source bar is still open
}

TEST_CASE("recipe versions have unique ordered columns and retain valid no-pattern zero", "[recipe][cdl]") {
    const auto legacy = recipe::projection_for(recipe::kRecipeVersion);
    const auto expanded = recipe::projection_for(recipe::kRecipeVersion2);
    CHECK(legacy == recipe::talib_projection());
    REQUIRE(expanded.size() > legacy.size());
    CHECK(std::equal(legacy.begin(), legacy.end(), expanded.begin()));
    CHECK(std::set<std::string>(expanded.begin(), expanded.end()).size() == expanded.size());
    CHECK(recipe::candlestick_projection().size() == 61);
    for (const auto& name : recipe::candlestick_projection()) {
        CHECK(std::count(expanded.begin(), expanded.end(), name) == 1);
    }
    CHECK(recipe::projection_for("unknown-recipe").empty());
}

TEST_CASE("fast windows preserve duration and refuse untyped or malformed source identities", "[recipe][fast]") {
    const auto source = training::parse_aligned_source("2:5s:1:book_ofi_receipt:1", 0);
    REQUIRE(source);
    CHECK(training::source_span_ms(*source) == 5'000);
    CHECK(training::aligned_source_spec(*source) == "2:5s:1:book_ofi_receipt:1");
    const auto sql = training::aligned_source_sql_pit(*source);
    CHECK(sql.find("interval '5000 milliseconds'") != std::string::npos);
    CHECK(training::aligned_observation_usable(*source, 595'000, 600'000));
    CHECK_FALSE(training::aligned_observation_usable(*source, 596'000, 600'000));
    for (const auto* invalid : {"2:5s:1", "1:5s:1:book_ofi_receipt:1", "2:5s:1:book_ofi_receipt:2",
            "2:5s:1:book_ofi_receipt:1garbage", "2:5s:1:book_ofi_receipt:1:0:trailing",
            "2:1m:1:bad'set:1", "2:1m:1:reference_lite:1;SELECT", "2:1m:1x"}) {
        CHECK_FALSE(training::parse_aligned_source(invalid, 0));
    }
    CHECK(recipe::is_excluded("book_ofi_receipt", "ofi_base"));
    CHECK_FALSE(recipe::is_excluded("book_ofi_receipt", "ofi_depth_normalized"));
    CHECK(recipe::is_excluded("book_ofi_receipt", "ofi_mean_depth_base"));
    CHECK(recipe::source_layout_compatible("book_ofi_receipt", 1,
        {"ofi_base", "ofi_depth_normalized", "ofi_event_count", "ofi_mean_depth_base"}));
    CHECK_FALSE(recipe::source_layout_compatible("book_ofi_receipt", 1,
        {"ofi_depth_normalized", "ofi_base", "ofi_event_count", "ofi_mean_depth_base"}));
    const auto macro = training::parse_aligned_source("0:1m:5:macro-core:1:0:mc-1.4.0", 0);
    REQUIRE(macro);
    CHECK(macro->engine_version == "mc-1.4.0");
    CHECK(training::aligned_source_spec(*macro) == "0:1m:5:macro-core:1:0:mc-1.4.0");
    CHECK(training::aligned_source_sql_pit(*macro).find("engine_version = 'mc-1.4.0'") != std::string::npos);
    CHECK_FALSE(training::parse_aligned_source("0:1m:5:macro-core:1:0:bad'engine", 0));
    CHECK_FALSE(recipe::source_method_compatible(recipe::kRecipeVersion2, "macro-core", ""));
    CHECK_FALSE(recipe::source_method_compatible(recipe::kRecipeVersion2, "macro-core", "mc-1.3.0"));
    CHECK(recipe::source_method_compatible(recipe::kRecipeVersion2, "macro-core", "mc-1.4.0"));
    CHECK(recipe::source_method_compatible(recipe::kRecipeVersion, "macro-core", ""));
    CHECK_FALSE(recipe::source_method_compatible(recipe::kRecipeVersion2, "unknown-family", "unknown-engine"));
}
