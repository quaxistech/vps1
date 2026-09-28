#include "altsignalquality/core/codec.hpp"

#include "altsignalquality/core/time.hpp"

namespace altsignalquality::messaging {

altsignalquality::v1::Envelope make_envelope(const EnvelopeMeta& meta) {
    altsignalquality::v1::Envelope env;
    env.set_schema_version(kSchemaVersion);
    env.set_message_id(meta.message_id);
    env.set_event_type(meta.event_type);
    env.set_canonical_pair_id(meta.canonical_pair_id);
    env.set_symbol(meta.symbol);
    env.set_produced_at_ms(timeutil::now_ms());
    env.set_trace_id(meta.trace_id);
    env.set_correlation_id(meta.correlation_id);
    env.set_source_service("altsignalquality");
    return env;
}

std::string serialize(const altsignalquality::v1::Envelope& env) {
    std::string out;
    env.SerializeToString(&out);
    return out;
}

Result<altsignalquality::v1::Envelope> parse_envelope(std::string_view bytes) {
    altsignalquality::v1::Envelope env;
    if (!env.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) {
        return fail(ErrorCode::parse_error, "invalid protobuf envelope");
    }
    if (env.schema_version() != kSchemaVersion) {
        return fail(ErrorCode::validation_error,
                    "unsupported schema version: " + std::to_string(env.schema_version()));
    }
    return env;
}

Result<altstrategy::v1::TradeSignalProposal> parse_candidate(std::string_view bytes,
                                                             std::string* symbol_out,
                                                             std::string* trace_id_out) {
    altstrategy::v1::Envelope env;
    if (!env.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) {
        return fail(ErrorCode::parse_error, "invalid altstrategy.v1 envelope");
    }
    if (env.schema_version() != kSchemaVersion) {
        return fail(ErrorCode::validation_error,
                    "unsupported altstrategy schema version: " +
                        std::to_string(env.schema_version()));
    }
    if (!env.has_trade_signal_proposal()) {
        return fail(ErrorCode::not_found, "envelope carries no trade_signal_proposal");
    }
    if (symbol_out != nullptr) *symbol_out = env.symbol();
    if (trace_id_out != nullptr) *trace_id_out = env.trace_id();
    return env.trade_signal_proposal();
}

} // namespace altsignalquality::messaging
