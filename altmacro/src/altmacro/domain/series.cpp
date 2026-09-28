#include "altmacro/domain/series.hpp"

#include <string_view>

namespace altmacro::domain {

const char* to_string(SeriesClass c) noexcept {
    switch (c) {
        case SeriesClass::market: return "MARKET";
        case SeriesClass::official_reference: return "OFFICIAL_REFERENCE";
        case SeriesClass::economic_release: return "ECONOMIC_RELEASE";
        case SeriesClass::derived: return "DERIVED";
    }
    return "UNKNOWN";
}

const char* to_string(QualityStatus q) noexcept {
    switch (q) {
        case QualityStatus::valid: return "VALID";
        case QualityStatus::stale: return "STALE";
        case QualityStatus::missing: return "MISSING";
        case QualityStatus::provider_error: return "PROVIDER_ERROR";
        case QualityStatus::market_closed: return "MARKET_CLOSED";
        case QualityStatus::partial: return "PARTIAL";
    }
    return "UNKNOWN";
}

const char* to_string(PriceBasis b) noexcept {
    switch (b) {
        case PriceBasis::bid: return "bid";
        case PriceBasis::ask: return "ask";
        case PriceBasis::mid: return "mid";
        case PriceBasis::last: return "last";
        case PriceBasis::close: return "close";
        case PriceBasis::fixing: return "fixing";
        case PriceBasis::not_applicable: return "not_applicable";
    }
    return "unknown";
}

const char* to_string(TimestampSemantics t) noexcept {
    switch (t) {
        case TimestampSemantics::bar_open: return "bar_open";
        case TimestampSemantics::bar_close: return "bar_close";
        case TimestampSemantics::point_in_time: return "point_in_time";
        case TimestampSemantics::observation_date: return "observation_date";
    }
    return "unknown";
}

std::optional<SeriesClass> series_class_from_string(std::string_view s) noexcept {
    if (s == "MARKET") return SeriesClass::market;
    if (s == "OFFICIAL_REFERENCE") return SeriesClass::official_reference;
    if (s == "ECONOMIC_RELEASE") return SeriesClass::economic_release;
    if (s == "DERIVED") return SeriesClass::derived;
    return std::nullopt;
}

std::optional<QualityStatus> quality_status_from_string(std::string_view s) noexcept {
    if (s == "VALID") return QualityStatus::valid;
    if (s == "STALE") return QualityStatus::stale;
    if (s == "MISSING") return QualityStatus::missing;
    if (s == "PROVIDER_ERROR") return QualityStatus::provider_error;
    if (s == "MARKET_CLOSED") return QualityStatus::market_closed;
    if (s == "PARTIAL") return QualityStatus::partial;
    return std::nullopt;
}

std::optional<PriceBasis> price_basis_from_string(std::string_view s) noexcept {
    if (s == "bid") return PriceBasis::bid;
    if (s == "ask") return PriceBasis::ask;
    if (s == "mid") return PriceBasis::mid;
    if (s == "last") return PriceBasis::last;
    if (s == "close") return PriceBasis::close;
    if (s == "fixing") return PriceBasis::fixing;
    if (s == "not_applicable") return PriceBasis::not_applicable;
    return std::nullopt;
}

} // namespace altmacro::domain
