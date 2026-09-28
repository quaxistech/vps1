#pragma once

// v2 strategy framework (§10, §11, §14). A strategy is PURE decision logic over
// a prepared DecisionContext + a bounded parameter set. It never touches IO and
// never picks a winner — the strategy_engine runs every eligible strategy and
// publishes every valid candidate. Registration is compile-time.

#include "altstrategy/engine/decision_types.hpp"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace altstrategy::engine {

// One tunable parameter with hard bounds (§33). Calibration may only move a
// value within [min,max] on the `step` grid — never outside.
struct StrategyParam {
    std::string name;
    std::string type = "double";
    double min = 0;
    double max = 0;
    double step = 0;
    double default_value = 0;
};

// Bounded parameter values a strategy is evaluated with.
struct ParamValues {
    std::string parameter_set_id = "default";
    std::map<std::string, double> values;
    [[nodiscard]] double get(const std::string& k, double def) const {
        const auto it = values.find(k);
        return it == values.end() ? def : it->second;
    }
};

// Static strategy metadata (§10). Immutable per (strategy_id, version).
struct StrategyMeta {
    std::string strategy_id;
    std::string strategy_version = "1.0.0";
    Family family = Family::unspecified;
    std::vector<Side> supported_sides{Side::long_, Side::short_};
    std::vector<HoldingClass> holding_classes{HoldingClass::intraday};
    // No regime lists. A setup used to be eligible only in "allowed" regimes
    // of the GLOBAL regime -- a statement AltTech makes about the last minutes
    // of 1m bars -- while the setups live on the 15m grid and state 1-5 hour
    // hypotheses: a 1m reading gating a 15m breakout is a timeframe
    // substitution, and "not in LOW_LIQUIDITY / CHAOTIC" is an execution
    // judgement that belongs to VPS2. The regimes are recorded on every
    // candidate (bitget / global / 15m / 1h) as context, never as a gate.
    std::vector<std::string> required_features;
    ContextQuality required_quality = ContextQuality::degraded_2_of_4;
    int64_t max_candidate_freq_ms = 60'000;
    int64_t cooldown_ms = 0;
    int min_history_bars = 30;
    std::string explanation_template;
    std::vector<StrategyParam> parameter_schema;
    Lifecycle default_lifecycle = Lifecycle::research;

    [[nodiscard]] ParamValues default_params() const {
        ParamValues p;
        p.parameter_set_id = "default";
        for (const auto& s : parameter_schema) p.values[s.name] = s.default_value;
        return p;
    }
};

// What a strategy emits before the engine attaches ids/scores/adaptive weight.
struct RawSignal {
    Side side = Side::unspecified;
    double raw_setup_score = 0;      // 0..1 setup conviction
    double entry_hint = 0;
    double invalidation_hint = 0;
    double target_hint = 0;
    HoldingClass holding_class = HoldingClass::intraday;
    int64_t expected_horizon_ms = 0;
    double cross_venue_confirmation = 0;
    std::vector<std::string> reason_codes;
    std::vector<std::string> warnings;
    // What the setup looked like, in measured terms: the distance past the
    // level in ATR, the volume anomaly, the venue agreement, the room to the
    // next zone... Everything a detector used to require of a setup beyond its
    // own definition is recorded here instead, so the model -- and VPS2 --
    // judge it from data rather than from a hand-set bound. Named, finite
    // values only; an unmeasured quantity is absent, never 0.
    std::map<std::string, double> descriptors;
};

class IStrategy {
public:
    virtual ~IStrategy() = default;
    [[nodiscard]] virtual const StrategyMeta& meta() const = 0;
    // Deterministic: same context+params => same result. nullopt = no setup.
    [[nodiscard]] virtual std::optional<RawSignal> evaluate(const DecisionContext& ctx,
                                                            const ParamValues& p) const = 0;
};

// Compile-time registry of the catalogue.
class StrategyRegistry {
public:
    using Factory = std::function<std::unique_ptr<IStrategy>()>;
    static StrategyRegistry& instance();
    void add(Factory f) { factories_.push_back(std::move(f)); }
    [[nodiscard]] std::vector<std::unique_ptr<IStrategy>> create_all() const {
        std::vector<std::unique_ptr<IStrategy>> out;
        out.reserve(factories_.size());
        for (const auto& f : factories_) out.push_back(f());
        return out;
    }

private:
    std::vector<Factory> factories_;
};

#define ALTSTRATEGY_V2_REGISTER_STRATEGY(cls)                                    \
    namespace {                                                          \
    const bool cls##_v2_registered = [] {                               \
        ::altstrategy::engine::StrategyRegistry::instance().add(                 \
            [] { return std::make_unique<cls>(); });                    \
        return true;                                                     \
    }();                                                                 \
    }

} // namespace altstrategy::engine
