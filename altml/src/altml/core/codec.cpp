#include "altml/core/codec.hpp"

#include "altml/core/feature_contract.hpp"
#include "altml/core/time.hpp"

namespace altml::messaging {

// Every envelope AltML emits is about the target venue: predictions are Bitget
// predictions. If the target ever moves, this breaks here rather than shipping
// envelopes labelled with a venue the model was not trained on.
static_assert(static_cast<int>(altml::v1::EXCHANGE_BITGET) == analytics::kTargetExchangeId,
              "the outgoing envelope's exchange must be the declared target venue");


altml::v1::Envelope make_envelope(const EnvelopeMeta& meta) {
    altml::v1::Envelope env;
    env.set_schema_version(kSchemaVersion);
    env.set_message_id(meta.message_id);
    env.set_event_type(meta.event_type);
    env.set_exchange(altml::v1::EXCHANGE_BITGET);
    env.set_instrument_id(meta.instrument_id);
    env.set_exchange_symbol(meta.exchange_symbol);
    env.set_exchange_timestamp_ms(meta.exchange_timestamp_ms);
    env.set_produced_at_ms(timeutil::now_ms());
    env.set_trace_id(meta.trace_id);
    env.set_source_service(meta.source_service);
    return env;
}

std::string serialize(const altml::v1::Envelope& env) {
    std::string out;
    env.SerializeToString(&out);
    return out;
}

Result<altml::v1::Envelope> parse_envelope(std::string_view bytes) {
    altml::v1::Envelope env;
    if (!env.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) {
        return fail(ErrorCode::parse_error, "invalid protobuf envelope");
    }
    if (env.schema_version() != kSchemaVersion) {
        return fail(ErrorCode::validation_error,
                    "unsupported schema version: " + std::to_string(env.schema_version()));
    }
    return env;
}

} // namespace altml::messaging

