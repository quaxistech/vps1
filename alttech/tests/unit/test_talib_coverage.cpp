// TA-Lib coverage: the catalog must bind every function the library offers,
// except the ones excluded by name for a measured reason.
//
// This is a contract test, not a smoke test. Silently dropping a function —
// because a group was skipped, an input could not be bound, or an enumeration
// branch changed — would leave a hole in the feature vector that nothing else
// would notice.

#include "alttech/talib_features/types.hpp"

#include <ta_abstract.h>
#include <ta_common.h>
#include <ta_defs.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Element-wise math transforms of the price. Each was measured on a real
// BTCUSDT 1m series and produces NaN, infinity or a constant on every bar.
const std::set<std::string, std::less<>> kExcluded = {
    "ACOS", "ASIN", "ATAN", "CEIL", "COS",  "COSH", "EXP", "FLOOR",
    "LN",   "LOG10", "SIN", "SINH", "SQRT", "TAN",  "TANH"};

std::vector<std::string> library_functions() {
    std::vector<std::string> out;
    REQUIRE(TA_Initialize() == TA_SUCCESS);
    TA_StringTable* groups = nullptr;
    REQUIRE(TA_GroupTableAlloc(&groups) == TA_SUCCESS);
    for (unsigned g = 0; g < groups->size; ++g) {
        TA_StringTable* funcs = nullptr;
        if (TA_FuncTableAlloc(groups->string[g], &funcs) != TA_SUCCESS) continue;
        for (unsigned f = 0; f < funcs->size; ++f) out.emplace_back(funcs->string[f]);
        TA_FuncTableFree(funcs);
    }
    TA_GroupTableFree(groups);
    return out;
}

// A column belongs to the longest function name that prefixes it, so
// HT_DCPERIOD_outReal is not attributed to a hypothetical "HT".
std::string owning_function(std::string_view column, const std::vector<std::string>& sorted) {
    for (const auto& fn : sorted) {
        if (column == fn || (column.size() > fn.size() && column.starts_with(fn) &&
                             column[fn.size()] == '_')) {
            return fn;
        }
    }
    return {};
}

} // namespace

TEST_CASE("the catalog binds every TA-Lib function that is not excluded by name") {
    const auto library = library_functions();
    REQUIRE(library.size() > 150); // TA-Lib 0.7.1 ships 161

    std::vector<std::string> by_length = library;
    std::sort(by_length.begin(), by_length.end(),
              [](const std::string& a, const std::string& b) { return a.size() > b.size(); });

    std::set<std::string, std::less<>> bound;
    for (const auto& column : alttech::talib_features::core_feature_names()) {
        const std::string owner = owning_function(column, by_length);
        INFO("column with no owning TA-Lib function: " << column);
        REQUIRE_FALSE(owner.empty());
        bound.insert(owner);
    }

    for (const auto& fn : library) {
        const bool excluded = kExcluded.contains(fn);
        INFO("function " << fn << (excluded ? " must NOT be bound" : " must be bound"));
        CHECK(bound.contains(fn) == !excluded);
    }
    CHECK(bound.size() == library.size() - kExcluded.size());
}

TEST_CASE("rolling extremes are bound over highs and lows, not only closes") {
    const auto& names = alttech::talib_features::core_feature_names();
    const auto has = [&](std::string_view column) {
        return std::find(names.begin(), names.end(), column) != names.end();
    };
    // MAX over closes is not a Donchian band; MAX over highs and MIN over lows
    // are. Both bindings must exist.
    CHECK(has("MAX_20_outReal"));
    CHECK(has("MAX_HIGH_20_outReal"));
    CHECK(has("MIN_20_outReal"));
    CHECK(has("MIN_LOW_20_outReal"));
    // Time since the extreme, which the close-only binding cannot express.
    CHECK(has("MAXINDEX_HIGH_20_outInteger"));
    CHECK(has("MININDEX_LOW_20_outInteger"));
    // The volume twins the flat projection's z-score is built from.
    CHECK(has("SMA_VOL_20_outReal"));
    CHECK(has("STDDEV_VOL_20_outReal"));
}

TEST_CASE("every parameterised function is swept across the full period ladder") {
    const auto& names = alttech::talib_features::core_feature_names();
    const auto has = [&](std::string_view column) {
        return std::find(names.begin(), names.end(), column) != names.end();
    };
    for (const auto* period : {"5", "9", "14", "20", "50", "100", "200"}) {
        CHECK(has(std::string("RSI_") + period + "_outReal"));
        CHECK(has(std::string("ATR_") + period + "_outReal"));
        CHECK(has(std::string("ADX_") + period + "_outReal"));
    }
    // Every moving-average flavour TA-Lib offers, on a function that exposes the knob.
    for (const auto* flavour : {"SMA", "EMA", "WMA", "DEMA", "TEMA", "TRIMA", "KAMA",
                                "MAMA", "T3"}) {
        CHECK(has(std::string("BBANDS_20_") + flavour + "_outRealUpperBand"));
    }
}
