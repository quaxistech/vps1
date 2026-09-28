#pragma once

#include "altml/core/error.hpp"
#include "system.pb.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace altml::messaging {

inline constexpr uint32_t kSchemaVersion = 1;

struct EnvelopeMeta {
    std::string message_id;
    std::string event_type;
    int64_t instrument_id = 0;
    std::string exchange_symbol;
    int64_t exchange_timestamp_ms = 0;
    std::string trace_id;
    std::string source_service;
};

altml::v1::Envelope make_envelope(const EnvelopeMeta& meta);
std::string serialize(const altml::v1::Envelope& env);
Result<altml::v1::Envelope> parse_envelope(std::string_view bytes);

} // namespace altml::messaging

