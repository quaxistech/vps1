#pragma once
// NATS stream, durable and subject names.
//
// AltSpotter owns the input streams and their subjects; AltTech creates only
// its own durable consumers on them and never mutates a stream. Every AltTech
// output subject lives under the owned `alttech.` namespace, so the single
// ALTTECH_ANALYTICS stream (subject filter `alttech.>`) captures all of them.
//
// The `.v1` suffix on output subjects is the public contract version, matching
// the `alttech.v1` protobuf package. It changes only on a breaking contract
// change, never on a release.

#include "alttech/domain/types.hpp"
#include "alttech/domain/venue.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

namespace alttech::messaging::topology {

// --- streams and durables --------------------------------------------------
inline constexpr std::string_view kDefaultInputStream = "ALTSPOTTER_BARS";
inline constexpr std::string_view kDefaultOutputStream = "ALTTECH_ANALYTICS";
inline constexpr std::string_view kDefaultBarConsumer = "alttech-bars";

// --- input subjects (owned by AltSpotter) ----------------------------------
// AltSpotter publishes closed bars for all four venues under `bar.>`. AltTech's
// durable filters down to the primary venue, because that is the only venue
// whose bars drive a live computation: the reference venues are read from
// AltSpotter's durable 1m PostgreSQL aggregates by the per-venue sweeps. Taking
// delivery of bars nothing consumes would only build an unacknowledged backlog.
inline constexpr std::string_view kBarSubject = "bar.bitget.>";
inline constexpr std::string_view kUniverseSubject = "evt.universe.>";

// --- output subject namespace ----------------------------------------------
inline constexpr std::string_view kAnalyticsSubject = "alttech.>";

// Restrict deployment-provided names to a portable subset accepted by both
// JetStream stream and durable-consumer names. This also keeps names safe in
// logs and administrative subjects.
[[nodiscard]] constexpr bool valid_resource_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > 255) return false;
    for (const char character : name) {
        const bool alpha = (character >= 'A' && character <= 'Z') ||
                           (character >= 'a' && character <= 'z');
        const bool digit = character >= '0' && character <= '9';
        if (!alpha && !digit && character != '_' && character != '-') return false;
    }
    return true;
}

// True when the NATS subject `pattern` captures everything `filter` captures.
//
// A stream AltTech reads is owned by AltSpotter and is configured with the
// broadest pattern it needs — `bar.>`. AltTech's own durable filters that down
// to one venue. Validating the input stream therefore has to ask whether the
// stream *covers* the subject AltTech wants, not whether the two strings are
// equal: requiring equality would reject a perfectly valid stream.
//
// `*` matches exactly one token; `>` matches the remaining tokens and may only
// appear last.
[[nodiscard]] constexpr bool subject_covers(std::string_view pattern,
                                            std::string_view filter) noexcept {
    std::size_t p = 0;
    std::size_t f = 0;
    while (p < pattern.size() && f < filter.size()) {
        const std::size_t p_end = std::min(pattern.find('.', p), pattern.size());
        const std::size_t f_end = std::min(filter.find('.', f), filter.size());
        const std::string_view p_token = pattern.substr(p, p_end - p);
        const std::string_view f_token = filter.substr(f, f_end - f);

        if (p_token == ">") return true; // pattern absorbs the rest
        if (f_token == ">") return false; // filter is broader than the pattern
        if (p_token != "*" && p_token != f_token) return false;

        p = p_end == pattern.size() ? pattern.size() : p_end + 1;
        f = f_end == filter.size() ? filter.size() : f_end + 1;
        if (p >= pattern.size() && f >= filter.size()) return true;
        if (p >= pattern.size() || f >= filter.size()) {
            // One side ran out of tokens: only a trailing `>` can still cover.
            return p < pattern.size() && pattern.substr(p) == ">";
        }
    }
    return p >= pattern.size() && f >= filter.size();
}

// --- input subject builders ------------------------------------------------
[[nodiscard]] inline std::string bar_closed_subject(Venue venue, std::string_view symbol,
                                                    Timeframe tf) {
    return std::string("bar.").append(venue_code(venue)).append(".").append(symbol).append(".")
        .append(tf_label(tf)).append(".closed");
}

[[nodiscard]] inline std::string trade_subject(Venue venue, std::string_view symbol) {
    return std::string("md.").append(venue_code(venue)).append(".").append(symbol)
        .append(".trade");
}

// --- per-venue output subjects ---------------------------------------------
// The profile segment distinguishes the primary venue's full vector from the
// reference venues' lighter one, so a consumer can subscribe to exactly one.
[[nodiscard]] inline std::string venue_feature_subject(Venue venue, std::string_view symbol,
                                                       Timeframe tf) {
    const std::string_view profile = is_primary(venue) ? "core" : "reference";
    return std::string("alttech.feature.venue.").append(venue_code(venue)).append(".")
        .append(symbol).append(".").append(tf_label(tf)).append(".").append(profile).append(".v1");
}

[[nodiscard]] inline std::string venue_microfeature_subject(Venue venue, std::string_view symbol,
                                                            std::string_view window) {
    return std::string("alttech.microfeature.venue.").append(venue_code(venue)).append(".")
        .append(symbol).append(".").append(window).append(".v1");
}

[[nodiscard]] inline std::string venue_structure_subject(Venue venue, std::string_view symbol,
                                                         Timeframe tf) {
    return std::string("alttech.structure.venue.").append(venue_code(venue)).append(".")
        .append(symbol).append(".").append(tf_label(tf)).append(".v1");
}

[[nodiscard]] inline std::string venue_structure_event_subject(Venue venue,
                                                               std::string_view symbol,
                                                               Timeframe tf,
                                                               std::string_view event) {
    return std::string("alttech.structure.venue.").append(venue_code(venue)).append(".")
        .append(symbol).append(".").append(tf_label(tf)).append(".").append(event);
}

[[nodiscard]] inline std::string venue_regime_subject(Venue venue, std::string_view symbol,
                                                      Timeframe tf) {
    return std::string("alttech.regime.venue.").append(venue_code(venue)).append(".")
        .append(symbol).append(".").append(tf_label(tf)).append(".v1");
}

[[nodiscard]] inline std::string venue_regime_combined_subject(Venue venue,
                                                               std::string_view symbol) {
    return std::string("alttech.regime.venue.").append(venue_code(venue)).append(".")
        .append(symbol).append(".combined.v1");
}

// --- cross-venue output subjects -------------------------------------------
[[nodiscard]] inline std::string cross_venue_feature_subject(std::string_view symbol,
                                                             Timeframe tf) {
    return std::string("alttech.feature.cross_venue.").append(symbol).append(".")
        .append(tf_label(tf)).append(".core.v1");
}

[[nodiscard]] inline std::string cross_venue_structure_subject(std::string_view symbol,
                                                               Timeframe tf) {
    return std::string("alttech.structure.cross_venue.").append(symbol).append(".")
        .append(tf_label(tf)).append(".v1");
}

[[nodiscard]] inline std::string bitget_projected_structure_subject(std::string_view symbol,
                                                                    Timeframe tf) {
    return std::string("alttech.structure.bitget_projected.").append(symbol).append(".")
        .append(tf_label(tf)).append(".v1");
}

[[nodiscard]] inline std::string cross_venue_regime_subject(std::string_view symbol,
                                                            Timeframe tf) {
    return std::string("alttech.regime.cross_venue.").append(symbol).append(".")
        .append(tf_label(tf)).append(".v1");
}

[[nodiscard]] inline std::string cross_venue_quality_subject(std::string_view symbol,
                                                             Timeframe tf) {
    return std::string("alttech.quality.cross_venue.").append(symbol).append(".")
        .append(tf_label(tf)).append(".v1");
}

// Cross-venue window lifecycle: ready | degraded | invalid.
[[nodiscard]] inline std::string cross_venue_event_subject(std::string_view symbol,
                                                           std::string_view kind) {
    return std::string("alttech.evt.cross_venue.").append(symbol).append(".").append(kind);
}

// --- global (instrument-independent) output subjects ----------------------
// The macro / cross-asset block belongs to no venue and no pair. It is one
// vector per minute for the whole system, published under the same
// `alttech.feature.` prefix the per-venue and cross-venue vectors use, so the
// one consumer subscription that carries feature vectors carries this too.
[[nodiscard]] inline std::string macro_feature_subject(Timeframe tf) {
    return std::string("alttech.feature.macro.global.").append(tf_label(tf)).append(".core.v1");
}

// --- module status subjects ------------------------------------------------
[[nodiscard]] inline std::string status_subject(std::string_view module, Venue venue,
                                                std::string_view symbol,
                                                std::string_view timeframe,
                                                std::string_view kind) {
    return std::string("alttech.status.").append(module).append(".").append(venue_code(venue))
        .append(".").append(symbol).append(".").append(timeframe).append(".").append(kind);
}

} // namespace alttech::messaging::topology
