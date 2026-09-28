#include "altmacro/components/publisher.hpp"

#include "altmacro/core/logger.hpp"
#include "altmacro/core/time.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace altmacro::components {

std::string Publisher::subject_token(const std::string& instrument_code) {
    std::string s = instrument_code;
    std::replace(s.begin(), s.end(), '.', '_');
    return s;
}

std::string Publisher::bar_subject(const std::string& instrument_code,
                                   domain::PriceBasis basis) const {
    return cfg_.subject_prefix + ".bar." + subject_token(instrument_code) + "." +
           domain::to_string(basis);
}

std::string Publisher::reference_subject(const std::string& instrument_code) const {
    return cfg_.subject_prefix + ".reference." + subject_token(instrument_code);
}

std::string Publisher::message_id(const data::SourceBinding& b, const std::string& timeframe,
                                  int64_t event_time_ms, domain::PriceBasis basis) {
    return b.instrument_code + "|" + b.provider_code + "|" + timeframe + "|" +
           domain::to_string(basis) + "|" + std::to_string(event_time_ms);
}

namespace {

// The provenance envelope every message carries.
nlohmann::json envelope(const data::SourceBinding& b, const char* kind,
                        const std::string& timeframe) {
    return nlohmann::json{
        {"schema_version", Publisher::kSchemaVersion},
        {"kind", kind},
        {"instrument", b.instrument_code},
        {"series_class", b.series_class},
        {"denomination", b.denomination},
        {"native_frequency", b.native_frequency},
        {"timeframe", timeframe},
        {"provider", b.provider_code},
        {"native_symbol", b.native_symbol},
        {"timestamp_semantics", b.timestamp_semantics},
        {"source_timezone", b.source_timezone},
        {"session_calendar", b.session_calendar},
    };
}

} // namespace

Result<int64_t> Publisher::publish_bars(const data::SourceBinding& b, const std::string& timeframe,
                                        const std::vector<domain::PriceBar>& bars,
                                        const Heartbeat& heartbeat) {
    if (!cfg_.enabled || nats_ == nullptr) return int64_t{0};
    const std::string subject = bar_subject(b.instrument_code, b.price_basis);
    const int64_t ingest = timeutil::now_ms();

    int64_t published = 0;
    for (const auto& bar : bars) {
        if (heartbeat && (published % 32) == 0 && heartbeat()) break;
        nlohmann::json msg = envelope(b, "bar", timeframe);
        msg["price_basis"] = domain::to_string(bar.basis);
        // Both clocks travel with the observation. A consumer that selects on
        // event_time alone can look ahead; ingest_time is what bounds when the
        // number could actually have been known here.
        msg["event_time"] = timeutil::to_iso8601(bar.event_time_ms);
        msg["event_time_ms"] = bar.event_time_ms;
        msg["ingest_time"] = timeutil::to_iso8601(ingest);
        msg["quality"] = domain::to_string(bar.quality);
        if (!bar.quality_reason.empty()) msg["quality_reason"] = bar.quality_reason;

        // Absent prices are absent, not zero. JSON null is the honest wire
        // form; a consumer that sees null cannot mistake it for a measurement.
        msg["open"] = bar.open ? nlohmann::json(*bar.open) : nlohmann::json(nullptr);
        msg["high"] = bar.high ? nlohmann::json(*bar.high) : nlohmann::json(nullptr);
        msg["low"] = bar.low ? nlohmann::json(*bar.low) : nlohmann::json(nullptr);
        msg["close"] = bar.close ? nlohmann::json(*bar.close) : nlohmann::json(nullptr);
        msg["volume"] = bar.volume ? nlohmann::json(*bar.volume) : nlohmann::json(nullptr);

        const std::string payload = msg.dump();
        auto r = nats_->js_publish(subject, payload,
                                   message_id(b, timeframe, bar.event_time_ms, bar.basis),
                                   cfg_.publish_timeout_ms);
        if (!r) return std::unexpected(r.error());
        ++published;
    }
    return published;
}

Result<int64_t> Publisher::publish_reference(const data::SourceBinding& b,
                                             const std::vector<domain::ReferenceValue>& values,
                                             const Heartbeat& heartbeat) {
    if (!cfg_.enabled || nats_ == nullptr) return int64_t{0};
    const std::string subject = reference_subject(b.instrument_code);
    const int64_t ingest = timeutil::now_ms();

    int64_t published = 0;
    for (const auto& v : values) {
        if (heartbeat && (published % 32) == 0 && heartbeat()) break;
        nlohmann::json msg = envelope(b, "reference", b.native_frequency);
        msg["event_time"] = timeutil::to_iso8601(v.event_time_ms);
        msg["event_time_ms"] = v.event_time_ms;
        msg["ingest_time"] = timeutil::to_iso8601(ingest);
        // Availability for a reference series is publication, not observation.
        // When the source states no release time this is null, and the consumer
        // must fall back to ingest_time — a bound, not a fiction.
        msg["publication_time"] = v.publication_time_ms
                                      ? nlohmann::json(timeutil::to_iso8601(*v.publication_time_ms))
                                      : nlohmann::json(nullptr);
        msg["quality"] = domain::to_string(v.quality);
        if (!v.quality_reason.empty()) msg["quality_reason"] = v.quality_reason;
        msg["value"] = v.value ? nlohmann::json(*v.value) : nlohmann::json(nullptr);
        if (v.value_as_of_ms) msg["value_as_of"] = timeutil::to_iso8601(*v.value_as_of_ms);

        const std::string payload = msg.dump();
        auto r = nats_->js_publish(subject, payload,
                                   message_id(b, b.native_frequency, v.event_time_ms,
                                              domain::PriceBasis::not_applicable),
                                   cfg_.publish_timeout_ms);
        if (!r) return std::unexpected(r.error());
        ++published;
    }
    return published;
}

} // namespace altmacro::components
