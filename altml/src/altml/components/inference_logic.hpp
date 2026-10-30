#pragma once

#include "altml/core/codec.hpp"
#include "feature.pb.h"
#include "system.pb.h"
#include "alttech/system.pb.h"
#include "altml/core/feature_contract.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace altml::inference {

// All dependency clocks and revisions travel with the cached values. Keeping
// multiple revisions lets a delayed target select the version available at its
// own cutoff rather than the last correction received by this process.
struct AlignedObservation {
    int64_t bar_open_ms = 0;
    int32_t feature_set_version = 0;
    std::vector<float> values;
    int64_t bar_revision = 0;
    int64_t computed_at_ms = 0;
    int64_t received_at_ms = 0;
    std::string engine_version{};
};

inline void record_aligned_revision(std::deque<AlignedObservation>& hist,
                                    AlignedObservation obs, size_t cap) {
    if (cap == 0) return;
    const auto key = [](const AlignedObservation& v) {
        return std::tuple{v.bar_open_ms, v.computed_at_ms, v.bar_revision,
                          v.received_at_ms, v.feature_set_version};
    };
    const auto it = std::lower_bound(hist.begin(), hist.end(), key(obs),
        [&](const AlignedObservation& v, const auto& want) { return key(v) < want; });
    if (it != hist.end() && key(*it) == key(obs)) return;
    if (hist.size() >= cap && it == hist.begin()) return;
    hist.insert(it, std::move(obs));
    while (hist.size() > cap) hist.pop_front();
}

// An empty string means the vector is safe to feed into the live model.
// Freshness is anchored to the source bar close and durable computation time,
// so replaying an old vector can never make historical data appear live.
inline std::string feature_rejection_reason(const altml::v1::FeatureVectorV1& fv,
                                            int64_t now_ms, int64_t stale_after_ms,
                                            const std::string& expected_schema_hash,
                                            int expected_feature_count) {
    constexpr int64_t kBarSpanMs = 60'000;
    constexpr int64_t kClockGraceMs = 5'000;
    if (stale_after_ms <= 0 || expected_feature_count <= 0 || now_ms <= 0) {
        return "invalid_gate_configuration";
    }
    if (fv.timeframe() != altml::v1::TF_1M || fv.bar_open_time_ms() < 0 ||
        fv.bar_open_time_ms() % kBarSpanMs != 0 ||
        fv.bar_open_time_ms() > std::numeric_limits<int64_t>::max() - kBarSpanMs) {
        return "invalid_bar_identity";
    }
    const int64_t bar_close_ms = fv.bar_open_time_ms() + kBarSpanMs;
    if (bar_close_ms > now_ms + kClockGraceMs || bar_close_ms < now_ms - stale_after_ms) {
        return "stale_or_future_bar";
    }
    if (fv.computed_at_ms() <= 0 || fv.computed_at_ms() > now_ms + kClockGraceMs ||
        fv.computed_at_ms() < now_ms - stale_after_ms) {
        return "stale_or_future_computation";
    }
    if (fv.status() != altml::v1::FEATURE_STATUS_VALID) return "feature_not_valid";
    if (fv.feature_set_name() != altml::analytics::kCoreFeatureSetName ||
        fv.feature_set_version() != altml::analytics::kCoreFeatureSetVersion ||
        fv.engine_version() != altml::analytics::kFeatureEngineVersion) {
        return "feature_contract_mismatch";
    }
    if (fv.schema_hash() != expected_schema_hash || fv.values_size() != expected_feature_count) {
        return "model_schema_mismatch";
    }
    const size_t expected_bitmap = (static_cast<size_t>(expected_feature_count) + 7U) / 8U;
    if (fv.validity_bitmap().size() != expected_bitmap) return "invalid_validity_bitmap";
    for (int i = 0; i < fv.values_size(); ++i) {
        const auto byte = static_cast<uint8_t>(fv.validity_bitmap()[static_cast<size_t>(i / 8)]);
        if (((byte >> (i % 8)) & 1U) != 0U && !std::isfinite(fv.values(i))) {
            return "non_finite_valid_feature";
        }
    }
    return {};
}

// The two status enums are different types with the same meanings. Mapping them
// by name keeps WARMING as WARMING: turning it into VALID would invent a
// readiness the producer never claimed.
inline altml::v1::FeatureVectorStatus map_analytics_status(alttech::v1::AnalyticsVectorStatus s) {
    switch (s) {
        case alttech::v1::ANALYTICS_VECTOR_STATUS_VALID:
            return altml::v1::FEATURE_STATUS_VALID;
        case alttech::v1::ANALYTICS_VECTOR_STATUS_NOT_READY:
            return altml::v1::FEATURE_STATUS_NOT_READY;
        case alttech::v1::ANALYTICS_VECTOR_STATUS_WARMING:
            return altml::v1::FEATURE_STATUS_WARMING;
        case alttech::v1::ANALYTICS_VECTOR_STATUS_INCOMPLETE_INPUT:
            return altml::v1::FEATURE_STATUS_INCOMPLETE_INPUT;
        case alttech::v1::ANALYTICS_VECTOR_STATUS_REVISED:
            return altml::v1::FEATURE_STATUS_REVISED;
        default:
            return altml::v1::FEATURE_STATUS_INVALID;
    }
}

// A cross-venue vector's validity lives in `quality_status`, not `status`.
//
// The producer fills quality_status (STRICT_VALID / DEGRADED_n_OF_4 / INVALID)
// and never calls set_status at all, so `status` arrives as UNSPECIFIED on every
// message. Mapping that field alone marked all 8514 consensus vectors in the
// stream INVALID, and an INVALID vector is skipped before archiving without a
// log line -- the data was consumed, decoded, and silently dropped.
//
// DEGRADED_n_OF_4 is not downgraded to INVALID and not promoted to VALID: it
// means the consensus stood on n of 4 venues, which is exactly INCOMPLETE_INPUT.
// It is preserved rather than archived as if it were whole.
inline altml::v1::FeatureVectorStatus map_cross_venue_quality(
    alttech::v1::CrossVenueQualityStatus q) {
    switch (q) {
        case alttech::v1::CROSS_VENUE_QUALITY_STRICT_VALID:
            return altml::v1::FEATURE_STATUS_VALID;
        case alttech::v1::CROSS_VENUE_QUALITY_DEGRADED_3_OF_4:
        case alttech::v1::CROSS_VENUE_QUALITY_DEGRADED_2_OF_4:
            return altml::v1::FEATURE_STATUS_INCOMPLETE_INPUT;
        default:
            return altml::v1::FEATURE_STATUS_INVALID;
    }
}

// Microstructure carries its own status enum. WARMING stays WARMING and
// NOT_AVAILABLE is not a quiet zero: both keep the vector out of the archive.
inline altml::v1::FeatureVectorStatus map_microstructure_status(
    alttech::v1::MicrostructureVectorStatus s) {
    switch (s) {
        case alttech::v1::MICROSTRUCTURE_STATUS_VALID:
            return altml::v1::FEATURE_STATUS_VALID;
        case alttech::v1::MICROSTRUCTURE_STATUS_WARMING:
            return altml::v1::FEATURE_STATUS_WARMING;
        default:
            return altml::v1::FEATURE_STATUS_INVALID;
    }
}

inline std::optional<altml::v1::Timeframe> window_timeframe(std::string_view window) {
    if (window == "1m") return altml::v1::TF_1M;
    if (window == "1s") return altml::v1::TF_1S;
    if (window == "5s") return altml::v1::TF_5S;
    if (window == "15s") return altml::v1::TF_15S;
    if (window == "30s") return altml::v1::TF_30S;
    return std::nullopt;
}

// The production decoder, in a header so the regression test can call it
// rather than a copy of it.
inline bool repack_feature_envelope(const alttech::v1::Envelope& in, altml::v1::Envelope* out) {
    // Every venue AltTech publishes is repacked. Bitget is the TARGET venue --
    // the thing we predict -- but Binance, OKX and Bybit are INFORMATION venues
    // whose observations are legitimate inputs as long as they are point-in-time
    // correct. Refusing them here is what kept the archive single-venue; the
    // model-input gate further down is what keeps the prediction Bitget's.
    //
    // Two payload shapes carry feature vectors on this namespace, and both are
    // approved parts of the feature universe:
    //
    //   FeatureVector      (tag 30) -- talib-core, the primary-venue engine
    //   VenueFeatureVector (tag 60) -- reference_lite and the other per-venue
    //                                  sets, published by the venue sweep
    //
    // The second was silently dropped as "unsupported payload" -- thousands of
    // messages a minute, terminated and gone. This decodes it into the same
    // canonical vector rather than growing a second decoder beside the first.
    const bool has_core = in.has_feature_vector();
    const bool has_venue = in.has_venue_feature_vector();
    const bool has_cross = in.has_cross_venue_feature_vector();
    const bool has_micro = in.has_microstructure_vector();
    const bool has_venue_micro = in.has_venue_microstructure_vector();
    if (in.schema_version() != messaging::kSchemaVersion ||
        (!has_core && !has_venue && !has_cross && !has_micro && !has_venue_micro)) {
        return false;
    }
    const int venue = static_cast<int>(in.exchange());
    // A cross-venue vector is the consensus of all four venues and belongs to
    // none of them, so its envelope carries EXCHANGE_UNSPECIFIED by design. The
    // macro block is the other global shape: one FeatureVector per minute for
    // the whole system, exchange unspecified AND instrument 0, because it is
    // the same observation for every target. Those are the two cases where an
    // unspecified exchange is meaningful rather than missing; for every other
    // shape it stays a refusal, because a guessed venue would file one venue's
    // observation under another.
    const bool global_core = has_core && in.instrument_id() == 0;
    if (has_cross || global_core) {
        if (venue != 0) return false;
    } else if (venue < 1 || venue > 4) {
        return false;
    }
    out->set_schema_version(in.schema_version());
    out->set_message_id(in.message_id());
    out->set_event_type(in.event_type());
    out->set_exchange(static_cast<altml::v1::Exchange>(venue));
    out->set_instrument_id(in.instrument_id());
    out->set_exchange_symbol(in.exchange_symbol());
    out->set_exchange_timestamp_ms(in.exchange_timestamp_ms());
    out->set_produced_at_ms(in.produced_at_ms());
    out->set_trace_id(in.trace_id());
    out->set_source_service(in.source_service());
    if (has_core) {
        // Field-for-field identical between the two packages; a lossless re-pack.
        return out->mutable_feature_vector()->ParseFromString(
            in.feature_vector().SerializeAsString());
    }

    // MicrostructureVector (tag 31) -- bitget_micro: trade counts and sizes,
    // aggressive buy/sell volume, delta and CVD, large-trade intensity,
    // short-term price impact, volume acceleration, burst score, book spread,
    // imbalance, microprice deviation, per-side depth. None of it is derivable
    // from a candle, and it is the execution venue's own book and tape -- the
    // most direct evidence about the cost of acting there. AltTech publishes it
    // per tumbling window on alttech.microfeature.venue.>; AltML's consumer did
    // not subscribe to that subject and its decoder had tag 31 RESERVED, so the
    // richest stream in the system (1.28M messages) reached nothing.
    //
    // Each fast window has a distinct internal timeframe identity. It is never
    // rounded into a minute; the source recipe keeps its exact millisecond span.
    if (has_micro) {
        const auto& m = in.microstructure_vector();
        const auto timeframe = window_timeframe(m.window_label());
        if (!timeframe) return false;
        auto* mv = out->mutable_feature_vector();
        mv->set_timeframe(*timeframe);
        mv->set_bar_open_time_ms(m.window_start_ms());
        mv->set_bar_revision(0);  // tumbling windows are not revised
        mv->set_feature_set_name(m.feature_set_name());
        mv->set_feature_set_version(m.feature_set_version());
        mv->set_schema_hash(m.schema_hash());
        mv->set_engine_version(m.engine_version());
        mv->mutable_values()->CopyFrom(m.values());
        mv->set_validity_bitmap(m.validity_bitmap());
        mv->set_computed_at_ms(m.computed_at_ms());
        mv->set_status(map_microstructure_status(m.status()));
        return true;
    }

    if (has_venue_micro) {
        const auto& m = in.venue_microstructure_vector();
        const auto timeframe = window_timeframe(m.window_label());
        if (!timeframe || static_cast<int>(m.exchange()) != venue ||
            m.canonical_pair_id() != in.canonical_pair_id()) return false;
        // A named true-OFI source cannot be substituted by sampled imbalance.
        if (m.feature_set_name() == "book_ofi_receipt" &&
            (m.feature_set_version() != 1 || m.profile() != "sequence_validated_delta" ||
             m.engine_version() != "alttech-ofi-1" || m.values_size() != 4 || venue == 1)) return false;
        if (m.feature_set_name() == "book_ofi_receipt" &&
            m.schema_hash() != "d10ab556ae283a0565fd19e9d3867b5001d97b9850332b49db593989ff7d6f93") return false;
        auto* mv = out->mutable_feature_vector();
        mv->set_timeframe(*timeframe);
        mv->set_bar_open_time_ms(m.window_start_ms());
        mv->set_bar_revision(0);
        mv->set_feature_set_name(m.feature_set_name());
        mv->set_feature_set_version(m.feature_set_version());
        mv->set_schema_hash(m.schema_hash());
        mv->set_engine_version(m.engine_version());
        mv->mutable_values()->CopyFrom(m.values());
        mv->set_validity_bitmap(m.validity_bitmap());
        mv->set_computed_at_ms(m.computed_at_ms());
        mv->set_status(map_analytics_status(m.status()));
        return true;
    }

    // CrossVenueFeatureVector (tag 61) -- cross_venue_core: consensus price,
    // mark and index, per-venue deviation from consensus, venue lead/lag
    // scores, order-flow and funding and open-interest agreement. AltTech has
    // published this all along on `alttech.feature.cross_venue.>` while AltML
    // subscribed only to `alttech.feature.venue.>`, so none of it was archived
    // and none of it could reach a model. It carries no bar_revision and no
    // engine_version: `analytics_revision` and `method_version` are the same
    // facts under the producer's own names, and they are carried across rather
    // than defaulted, because a revision silently pinned to 0 would make two
    // different computations of one bar collide on the archive's primary key.
    if (has_cross) {
        const auto& c = in.cross_venue_feature_vector();
        auto* xv = out->mutable_feature_vector();
        xv->set_timeframe(static_cast<altml::v1::Timeframe>(static_cast<int>(c.timeframe())));
        xv->set_bar_open_time_ms(c.bar_open_time_ms());
        xv->set_bar_revision(c.analytics_revision());
        xv->set_feature_set_name(c.feature_set_name());
        xv->set_feature_set_version(c.feature_set_version());
        xv->set_schema_hash(c.schema_hash());
        xv->set_engine_version(c.method_version());
        xv->mutable_values()->CopyFrom(c.values());
        xv->set_validity_bitmap(c.validity_bitmap());
        xv->set_computed_at_ms(c.computed_at_ms());
        // Prefer the field the producer actually fills, and keep honouring
        // `status` if it ever starts filling it.
        xv->set_status(c.status() == alttech::v1::ANALYTICS_VECTOR_STATUS_UNSPECIFIED
                           ? map_cross_venue_quality(c.quality_status())
                           : map_analytics_status(c.status()));
        return true;
    }

    // VenueFeatureVector is a superset: it carries the venue, the scope and the
    // profile in addition to everything FeatureVectorV1 holds. The venue is
    // already on the envelope, so this projects the rest across explicitly
    // rather than by re-serialisation -- the tags differ, and a byte copy here
    // would silently mis-assign fields.
    const auto& v = in.venue_feature_vector();
    auto* fv = out->mutable_feature_vector();
    fv->set_timeframe(static_cast<altml::v1::Timeframe>(static_cast<int>(v.timeframe())));
    fv->set_bar_open_time_ms(v.bar_open_time_ms());
    fv->set_bar_revision(v.bar_revision());
    fv->set_feature_set_name(v.feature_set_name());
    fv->set_feature_set_version(v.feature_set_version());
    fv->set_schema_hash(v.schema_hash());
    fv->set_engine_version(v.engine_version());
    fv->mutable_values()->CopyFrom(v.values());
    fv->set_validity_bitmap(v.validity_bitmap());
    fv->set_computed_at_ms(v.computed_at_ms());
    fv->set_status(map_analytics_status(v.status()));
    return true;
}

// Files one point-in-time observation into a history kept in BAR order, at
// most `cap` long, and returns nothing: the history is what latest_aligned()
// walks from the back to find the newest usable bar.
//
// The history used to be kept in ARRIVAL order. AltTech's cross-venue sweep
// computes the newest missing minute first and backfills the gaps behind it on
// later passes, publishing each, so older bars arrive after newer ones. An
// arrival-ordered history then held a backfilled minute at its back -- the
// "newest" to latest_aligned() -- and, the history being eight long, a burst of
// backfilled minutes pushed the genuinely newest bars out of the front.
template <typename Observation>
void record_in_bar_order(std::deque<Observation>& hist, Observation obs, size_t cap) {
    const auto it = std::lower_bound(
        hist.begin(), hist.end(), obs.bar_open_ms,
        [](const Observation& e, int64_t bar) { return e.bar_open_ms < bar; });
    if (it != hist.end() && it->bar_open_ms == obs.bar_open_ms) {
        *it = std::move(obs);   // a later revision of a bar already held
        return;
    }
    // Older than everything a full history keeps: it would be evicted at once.
    if (hist.size() >= cap && it == hist.begin()) return;
    hist.insert(it, std::move(obs));
    while (hist.size() > cap) hist.pop_front();
}

} // namespace altml::inference
