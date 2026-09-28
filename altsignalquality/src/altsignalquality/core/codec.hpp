#pragma once

#include "altsignalquality/core/error.hpp"
#include "signalquality.pb.h"
#include "vendor/decision.pb.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace altsignalquality::messaging {

inline constexpr uint32_t kSchemaVersion = 1;

struct EnvelopeMeta {
    std::string message_id;
    std::string event_type;
    int64_t canonical_pair_id = 0;
    std::string symbol;
    std::string trace_id;
    std::string correlation_id;
};

altsignalquality::v1::Envelope make_envelope(const EnvelopeMeta& meta);
std::string serialize(const altsignalquality::v1::Envelope& env);
Result<altsignalquality::v1::Envelope> parse_envelope(std::string_view bytes);

// Parse an inbound altstrategy.v1 envelope and return the candidate proposal it
// carries. A message that is not a TradeSignalProposal is not an error for this
// service — the subject filter is a contract, but a stream can legitimately
// carry other AltStrategy payloads — so absence is reported as `not_found`.
Result<altstrategy::v1::TradeSignalProposal> parse_candidate(std::string_view bytes,
                                                             std::string* symbol_out,
                                                             std::string* trace_id_out);

} // namespace altsignalquality::messaging
