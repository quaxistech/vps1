#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>

namespace alttech::structure {

struct ProfileArea {
    std::size_t poc = 0, low = 0, high = 0;
    double total_volume = 0, area_volume = 0;
};

// Applies to either honestly declared candle bins or a coverage-qualified
// trade profile. POC ties use the lower price; value-area ties expand upward.
template<std::size_t N>
[[nodiscard]] std::optional<ProfileArea> profile_area(const std::array<double, N>& volume,
                                                     double fraction = 0.7) {
    static_assert(N > 0);
    if (!std::isfinite(fraction) || fraction <= 0 || fraction > 1) return std::nullopt;
    ProfileArea area;
    for (double v : volume) {
        if (!std::isfinite(v) || v < 0) return std::nullopt;
        area.total_volume += v;
    }
    if (!(area.total_volume > 0) || !std::isfinite(area.total_volume)) return std::nullopt;
    area.poc = static_cast<std::size_t>(std::max_element(volume.begin(), volume.end()) - volume.begin());
    area.low = area.high = area.poc;
    area.area_volume = volume[area.poc];
    while (area.area_volume < fraction * area.total_volume && (area.low > 0 || area.high + 1 < N)) {
        const double above = area.high + 1 < N ? volume[area.high + 1] : -1;
        const double below = area.low > 0 ? volume[area.low - 1] : -1;
        if (above >= below) area.area_volume += volume[++area.high];
        else area.area_volume += volume[--area.low];
    }
    return area;
}

} // namespace alttech::structure
