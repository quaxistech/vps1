#pragma once

// Point-in-time context serialization for decision_context_snapshots.snapshot.
//
// Kept in a header, out of decision_store.cpp, so the encoding is unit-testable
// without a database: this JSON is what downstream point-in-time consumers
// reconstruct the state at T0 from, and a malformed or lossy encoding here is
// invisible until it has already been written for weeks.

#include "altstrategy/engine/decision_types.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <system_error>
#include <map>
#include <optional>
#include <string>
#include <utility>

namespace altstrategy::engine_io {

// A JSON object from a name -> value map, safe for a JSONB column.
//
// Three things the obvious `os << v` gets wrong, all of which fail silently
// until they do not: a non-finite double streams as `nan`/`inf`, which JSONB
// rejects and which aborts the whole transaction; the default 6 significant
// digits truncate a price (43567.89 -> 43567.9), which is not the number the
// decision was taken on; and both `<<` and `snprintf` honour the process
// LOCALE, so under a decimal-comma locale this would emit `43567,89` and
// produce malformed JSON. `std::to_chars` is locale-independent by definition
// and emits the shortest representation that round-trips exactly.
//
// Non-finite entries are omitted — a value that is not a number must be ABSENT
// downstream, never a plausible substitute.
inline std::string json_of(const std::map<std::string, double>& m) {
    std::string out = "{";
    bool first = true;
    char buf[64];
    for (const auto& [k, v] : m) {
        if (!std::isfinite(v)) continue;
        const auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), v,
                                             std::chars_format::general);
        if (ec != std::errc{}) continue;
        if (!first) out += ',';
        first = false;
        out += '"';
        out += k;
        out += "\":";
        out.append(buf, static_cast<size_t>(ptr - buf));
    }
    out += '}';
    return out;
}

// The point-in-time inputs the decision was actually taken on, for
// decision_context_snapshots.snapshot — a column the schema has always declared
// as "the immutable full snapshot of the inputs available at creation" and that
// nothing wrote. Downstream point-in-time consumers (SignalQuality) can only
// reconstruct the state at T0 if it is here; the scalar columns alone describe
// the venue, not the analytics the strategies read.
//
// Only the NAMED analytics the engine decoded are stored, not the producer's
// raw ordered vector: those are the quantities that have a meaning without the
// producer's feature-set catalogue, and they keep the row at roughly 2 KB
// against the table's 14-day retention.
// The scalar columns of decision_context_snapshots, NULL where the value was
// not observed. The context carries conservative stand-ins for an unobserved
// spread (10 bps) and depth (5 000 USDT), 0 for an absent liquidity rank, and 0
// for a funding rate that did not arrive -- each with its own flag or presence.
// Written as numbers, they reached the Gateway as MEASUREMENTS: it presence-gates
// on NULL ("AltStrategy writes NULL for not measured"), so a stand-in spread was
// delivered to the executor as a real one. Over seven days: 58 contexts with a
// 0 funding rate (14 of them with no funding reading at all) and 26 with a 0
// liquidity rank.
struct ObservedScalars {
    std::optional<double> funding_rate, spread_bps, top_depth_usdt, liquidity_rank;
};
[[nodiscard]] inline ObservedScalars observed_scalars(const engine::DecisionContext& c) {
    ObservedScalars s;
    if (c.cross_venue_features.has("funding_rate_8h")) s.funding_rate = c.funding_rate;
    if (c.spread_observed) s.spread_bps = c.spread_bps;
    if (c.depth_observed) s.top_depth_usdt = c.top_depth_usdt;
    if (c.liquidity_rank >= 1.0) s.liquidity_rank = c.liquidity_rank;
    return s;
}

inline std::string context_snapshot_json(const engine::DecisionContext& c) {
    const auto regime_json = [](const engine::RegimeView& r) {
        std::string out = "{\"regime\":\"" + r.regime + "\"";
        char buf[64];
        for (const auto& [name, value] :
             {std::pair<const char*, double>{"confidence", r.confidence},
              {"trend_strength", r.trend_strength},
              {"volatility_regime", r.volatility_regime},
              {"liquidity_quality", r.liquidity_quality}}) {
            if (!std::isfinite(value)) continue;
            const auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value,
                                                 std::chars_format::general);
            if (ec != std::errc{}) continue;
            out += ",\"";
            out += name;
            out += "\":";
            out.append(buf, static_cast<size_t>(ptr - buf));
        }
        return out + "}";
    };
    return "{\"bitget_features\":" + json_of(c.bitget_features.values) +
           ",\"microstructure\":" + json_of(c.microstructure.values) +
           ",\"cross_venue_features\":" + json_of(c.cross_venue_features.values) +
           ",\"reference_features\":" + [&] {
               // Keyed by venue so a reader can tell whose reading this is.
               // Flattening them would restate the merge this layer removed.
               std::string o = "{";
               bool first = true;
               for (const auto& [venue, s] : c.reference_features) {
                   if (!first) o += ",";
                   first = false;
                   o += "\"" + std::to_string(venue) + "\":" + json_of(s.values);
               }
               return o + "}";
           }() +
           ",\"context_features\":" + [&] {
               // Keyed by bar length in minutes, with each grid's own bar open
               // time, so a reader can check the closed-bar rule themselves
               // rather than trust that it was applied.
               std::string o = "{";
               bool first = true;
               for (const auto& [tf, s] : c.context_features) {
                   if (!first) o += ",";
                   first = false;
                   o += "\"" + std::to_string(tf) + "\":{\"bar_open_time_ms\":" +
                        std::to_string(s.bar_open_time_ms) + ",\"values\":" + json_of(s.values) + "}";
               }
               return o + "}";
           }() +
           ",\"bitget_regime_view\":" + regime_json(c.bitget_regime) +
           ",\"global_regime_view\":" + regime_json(c.global_regime) +
           // The venue grid the plans of this context were rounded to; absent
           // when no instrument spec resolved at T0 (the plans then say
           // tick_source = derived).
           (c.price_tick > 0 ? ",\"price_grid\":" + json_of({{"tick_size", c.price_tick},
                                                            {"instrument_spec_version",
                                                             static_cast<double>(c.instrument_spec_version)}})
                             : std::string()) +
           "}";
}

} // namespace altstrategy::engine_io
