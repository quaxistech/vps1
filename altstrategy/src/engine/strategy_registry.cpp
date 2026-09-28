#include "altstrategy/engine/strategy.hpp"

namespace altstrategy::engine {

StrategyRegistry& StrategyRegistry::instance() {
    static StrategyRegistry registry;
    return registry;
}

} // namespace altstrategy::engine
