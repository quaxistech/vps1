#pragma once

// Confluence-weighted merging of structure-zone candidates.
//
// Split out so the merge can be tested directly: the weight used to be computed
// inside the module and thrown away (`(void)wsum`), so a level confirmed by
// five methods merged with a single-method level as an equal and the resulting
// centre drifted halfway toward the weaker candidate. AltStrategy reads these
// centres as price levels.

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace alttech::structure {

struct ZoneCandidate {
    double lower = 0, center = 0, upper = 0;
    std::string kind;                       // support | resistance | dynamic
    std::map<std::string, double> methods;  // method -> weight
    int touch_count = 0;
    double volume_weight = 0;
};

// Summed method weight: what score_zone() also calls confluence, so merging and
// scoring agree on what "stronger" means. A candidate always counts at least
// once, so a zone with no recorded method still participates.
[[nodiscard]] inline double zone_weight(const ZoneCandidate& c) {
    double w = 0;
    for (const auto& [_, v] : c.methods) w += v;
    return w > 0.0 ? w : 1.0;
}

// Merge overlapping candidates of the same kind whose centres sit within half a
// zone width. The centre is a confluence-weighted mean, never a plain average.
[[nodiscard]] inline std::vector<ZoneCandidate> merge_zone_candidates(
    std::vector<ZoneCandidate> candidates, double half_width) {
    std::sort(candidates.begin(), candidates.end(),
              [](const ZoneCandidate& a, const ZoneCandidate& b) { return a.center < b.center; });
    std::vector<ZoneCandidate> merged;
    for (auto& z : candidates) {
        if (!merged.empty() && std::abs(merged.back().center - z.center) <= half_width &&
            merged.back().kind == z.kind) {
            auto& m = merged.back();
            const double wm = zone_weight(m);
            const double wz = zone_weight(z);
            m.center = (m.center * wm + z.center * wz) / (wm + wz);
            m.lower = m.center - half_width;
            m.upper = m.center + half_width;
            for (const auto& [k, v] : z.methods) m.methods[k] += v;
            m.touch_count += z.touch_count;
            m.volume_weight += z.volume_weight;
        } else {
            merged.push_back(std::move(z));
        }
    }
    return merged;
}

}  // namespace alttech::structure
