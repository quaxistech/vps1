#pragma once

// The label spec: what "this signal had a positive outcome" means, exactly.
//
// AltStrategy already owns outcome computation — it reconstructs every
// proposal's price path from AltSpotter history, classifies it, and refuses to
// guess an ambiguous one (docs/MARKET_OUTCOMES.md). AltSignalQuality does NOT
// build a second outcome system; it defines only the binary question its
// classifier answers, on top of the categorical outcome and the net return
// AltStrategy already computed.
//
// Two exclusions carry the whole design:
//
//   * AMBIGUOUS / DATA_INCOMPLETE — AltStrategy could not determine whether the
//     stop or the target came first. Scoring these either way manufactures a
//     label out of missing data, so they are excluded (this is also exactly
//     AltStrategy's own `binary_label_usable = false`).
//   * ENTRY_NOT_REACHED / ENTRY_EXPIRED — no position was ever taken, so there
//     is no outcome to be right or wrong about. Counting an unfilled signal as
//     a loss would train the filter on entry geometry, which belongs to
//     AltStrategy's plan generator; this mirrors the exclusion AltStrategy
//     itself applies when estimating strategy edge.
//
// What remains — TARGET_FIRST, STOP_FIRST, TIME_STOP — is the set of signals
// that actually took a position and resolved. The label is economic, not
// categorical: a TIME_STOP that closed above costs is a win, and a TARGET_FIRST
// whose costs ate the move is not. `net_return_after_costs` is AltStrategy's
// own cost-adjusted number, so the filter optimises the same currency the
// desk does.

#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>

namespace altsignalquality::domain {

inline constexpr std::string_view kLabelSpecVersion = "sq-label:1";

// Outcomes that took a position and resolved unambiguously. This array is the
// ONE definition: `outcome_is_labelable`, the label-spec JSON and the SQL the
// dataset builder issues are all derived from it, so the set cannot drift
// between the query and the code that interprets its rows.
inline constexpr std::array<std::string_view, 3> kLabelableOutcomes = {
    "TARGET_FIRST", "STOP_FIRST", "TIME_STOP"};

[[nodiscard]] constexpr bool outcome_is_labelable(std::string_view categorical) noexcept {
    for (const auto& o : kLabelableOutcomes) {
        if (o == categorical) return true;
    }
    return false;
}

// The SQL literal list for the dataset query: ('TARGET_FIRST','STOP_FIRST',...).
// Built from the array above; the tokens are compile-time constants from this
// header, never user input.
[[nodiscard]] inline std::string labelable_outcomes_sql_list() {
    std::string out = "(";
    for (size_t i = 0; i < kLabelableOutcomes.size(); ++i) {
        if (i != 0) out += ',';
        out += '\'';
        out += kLabelableOutcomes[i];
        out += '\'';
    }
    out += ')';
    return out;
}

struct OutcomeRecord {
    std::string categorical;          // AltStrategy's MarketOutcomeType name
    double net_return_after_costs = 0;
    bool binary_label_usable = true;  // AltStrategy's own flag
    bool net_return_present = false;
};

struct LabelResult {
    bool usable = false;
    int label = 0;                    // 1 = positive net outcome, 0 = not
    std::string rejection;            // why it is not usable (empty when it is)
};

[[nodiscard]] inline LabelResult label_of(const OutcomeRecord& o) {
    LabelResult out;
    if (!o.binary_label_usable) {
        out.rejection = "upstream_binary_label_unusable";
        return out;
    }
    if (!outcome_is_labelable(o.categorical)) {
        out.rejection = "outcome_took_no_position_or_ambiguous:" + o.categorical;
        return out;
    }
    if (!o.net_return_present || !std::isfinite(o.net_return_after_costs)) {
        out.rejection = "net_return_missing";
        return out;
    }
    out.usable = true;
    out.label = o.net_return_after_costs > 0.0 ? 1 : 0;
    return out;
}

// The machine-readable definition stored in altsignalquality.label_specs and
// hashed into every model. Changing any word here is a new label spec version.
[[nodiscard]] inline std::string label_spec_json() {
    return R"({"version":"sq-label:1",)"
           R"("source":"altstrategy.signal_training_labels + altstrategy.market_outcomes",)"
           R"("labelable_outcomes":["TARGET_FIRST","STOP_FIRST","TIME_STOP"],)"
           R"("excluded_outcomes":["ENTRY_NOT_REACHED","ENTRY_EXPIRED","CANCELLED",)"
           R"("AMBIGUOUS","DATA_INCOMPLETE"],)"
           R"("requires":["binary_label_usable=true","net_return_after_costs IS NOT NULL"],)"
           R"("positive_when":"net_return_after_costs > 0",)"
           R"("rationale":"economic, cost-adjusted, and never inferred from an outcome )"
           R"(AltStrategy declined to determine"})";
}

} // namespace altsignalquality::domain
