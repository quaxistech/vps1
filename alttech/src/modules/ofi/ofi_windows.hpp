#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace alttech::ofi {

// Input is computed by AltSpotter from every sequence/checksum-valid delta,
// before sampled book storage. This is not trade delta or static imbalance.
struct Event {
    std::string connection;
    uint64_t epoch = 0, counter = 0;
    int64_t received_ms = 0;
    double flow_base = 0, depth_base = 0;
    bool available = false;
};
struct Window {
    int64_t start_ms = 0, span_ms = 0;
    double flow_base = 0, depth_normalized = 0, mean_depth_base = 0;
    uint64_t updates = 0;
    bool valid = false;
};

class Windows {
public:
    static constexpr std::array<int64_t, 5> spans = {1'000, 5'000, 15'000, 30'000, 60'000};

    void add(const Event& e) {
        if (e.received_ms <= 0 || e.connection.empty() || e.epoch == 0 || e.counter == 0) return;
        const bool same = e.connection == connection_ && e.epoch == epoch_;
        // An exact duplicate cannot contribute twice. A regression breaks the
        // run; it must not silently reuse a future counter as past evidence.
        if (same && e.counter == counter_) return;
        const bool continuous = same && e.counter > counter_ && e.counter - counter_ == 1;
        if (!continuous) {
            for (auto& bins : bins_) for (auto& [_, bin] : bins) bin.complete = false;
        }
        connection_ = e.connection;
        epoch_ = e.epoch;
        counter_ = e.counter;
        observed_through_ms_ = std::max(observed_through_ms_, e.received_ms);
        const bool usable = e.available && std::isfinite(e.flow_base) &&
                            std::isfinite(e.depth_base) && e.depth_base > 0;
        for (size_t i = 0; i < spans.size(); ++i) {
            const int64_t start = e.received_ms / spans[i] * spans[i];
            if (start + spans[i] <= closed_through_[i]) continue;
            auto& bin = bins_[i][start];
            bin.complete = bin.complete && continuous && usable;
            ++bin.updates;
            if (usable) {
                bin.flow += e.flow_base;
                bin.depth += e.depth_base;
            }
            // Bounded even if the supervised tick stops while NATS continues.
            if (bins_[i].size() > 128) {
                closed_through_[i] = bins_[i].begin()->first + spans[i];
                bins_[i].erase(bins_[i].begin());
                for (auto& [_, remaining] : bins_[i]) remaining.complete = false;
            }
        }
    }

    [[nodiscard]] std::vector<Window> close(int64_t now_ms) {
        std::vector<Window> out;
        for (size_t i = 0; i < spans.size(); ++i) {
            auto& bins = bins_[i];
            // A subsequent chained delta proves that no trailing event was
            // silently lost before this boundary. Wall time alone cannot do
            // that on an ephemeral transport; an idle stream stays absent.
            while (!bins.empty() && bins.begin()->first + spans[i] <= now_ms &&
                   bins.begin()->first + spans[i] <= observed_through_ms_) {
                const auto [start, bin] = *bins.begin();
                const double depth = bin.updates > 0 ? bin.depth / static_cast<double>(bin.updates) : 0;
                const double normalized = depth > 0 ? bin.flow / depth : 0;
                out.push_back({start, spans[i], bin.flow, normalized, depth, bin.updates,
                               bin.complete && bin.updates > 0 && depth > 0 &&
                                   std::isfinite(bin.flow) && std::isfinite(depth) && std::isfinite(normalized)});
                closed_through_[i] = start + spans[i];
                bins.erase(bins.begin());
            }
        }
        return out;
    }

private:
    struct Bin { double flow = 0, depth = 0; uint64_t updates = 0; bool complete = true; };
    std::array<std::map<int64_t, Bin>, 5> bins_;
    std::array<int64_t, 5> closed_through_{};
    std::string connection_;
    uint64_t epoch_ = 0, counter_ = 0;
    int64_t observed_through_ms_ = 0;
};

} // namespace alttech::ofi
