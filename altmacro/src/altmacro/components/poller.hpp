#pragma once

#include "altmacro/components/publisher.hpp"
#include "altmacro/core/config.hpp"
#include "altmacro/core/error.hpp"
#include "altmacro/data/store.hpp"
#include "altmacro/providers/registry.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <string>

namespace altmacro::components {

// Keeps the stored series current.
//
// Both of our intraday sources are archives: Dukascopy writes its files after
// the fact and states no latency at all. Presenting the most recent bar as a
// live quote would promise something the source does not offer, so the poller
// re-reads a short recent window on a timer and lets the upsert do the rest.
// The service therefore claims freshness, not real time.
class Poller {
public:
    Poller(const Config& cfg, providers::Registry& registry, data::Store& store,
           Publisher* publisher)
        : cfg_(cfg), registry_(registry), store_(store), publisher_(publisher) {}

    struct Tick {
        int64_t bars_written = 0;
        int64_t reference_written = 0;
        int64_t published = 0;
        int64_t provider_failures = 0;
    };

    // One pass over everything due. Never throws and never aborts the loop on a
    // single provider's failure: one unavailable source must not stop the rest.
    [[nodiscard]] Tick poll_once(const std::function<bool()>& should_stop);

private:
    const Config& cfg_;
    providers::Registry& registry_;
    data::Store& store_;
    Publisher* publisher_;
    std::map<std::string, int64_t> last_run_ms_;

    [[nodiscard]] bool due(const std::string& key, int32_t interval_sec);
};

} // namespace altmacro::components
