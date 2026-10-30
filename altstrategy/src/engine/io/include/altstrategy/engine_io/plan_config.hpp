#pragma once
#include "altstrategy/common/error.hpp"
#include "altstrategy/engine/trade_plan.hpp"
#include <yaml-cpp/yaml.h>

namespace altstrategy::engine_io {
// Optional configuration has a default only when absent. A supplied malformed
// or out-of-range integer must never silently restore the default policy.
inline Result<int64_t> parse_max_holding_ms(const YAML::Node& plan) {
    constexpr int64_t hour_ms = 3'600'000;
    int64_t hours = engine::kMaxHoldingMs / hour_ms;
    try {
        if (plan && plan["max_holding_hours"]) hours = plan["max_holding_hours"].as<int64_t>();
    } catch (const std::exception&) {
        return fail(ErrorCode::config_error, "trade_plan.max_holding_hours must be an integer in [1,48]");
    }
    if (hours < 1 || hours > engine::kMaxHoldingMs / hour_ms)
        return fail(ErrorCode::config_error, "trade_plan.max_holding_hours must be in [1,48]");
    return hours * hour_ms; // range checked before multiplication
}
} // namespace altstrategy::engine_io
