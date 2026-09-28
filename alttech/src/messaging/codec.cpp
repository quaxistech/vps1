// Stable alttech.v1 wire codec implementation.
#include "alttech/messaging/codec.hpp"

#include "alttech/time/time.hpp"

namespace alttech::messaging {

::alttech::v1::Decimal to_proto(const Decimal& d) {
    ::alttech::v1::Decimal out;
    out.set_mantissa(d.mantissa());
    out.set_scale(d.scale());
    return out;
}

Decimal from_proto(const ::alttech::v1::Decimal& d) {
    return Decimal{d.mantissa(), d.scale()};
}

void to_proto(const Candle& c, ::alttech::v1::Candle* out) {
    out->set_timeframe(static_cast<::alttech::v1::Timeframe>(tf_minutes(c.timeframe)));
    out->set_open_time_ms(c.open_time_ms);
    out->set_close_time_ms(c.close_time_ms);
    *out->mutable_open() = to_proto(c.open);
    *out->mutable_high() = to_proto(c.high);
    *out->mutable_low() = to_proto(c.low);
    *out->mutable_close() = to_proto(c.close);
    *out->mutable_base_volume() = to_proto(c.base_volume);
    *out->mutable_quote_volume() = to_proto(c.quote_volume);
    if (c.trade_count) out->set_trade_count(*c.trade_count);
    if (c.taker_buy_base_volume) *out->mutable_taker_buy_base_volume() = to_proto(*c.taker_buy_base_volume);
    if (c.taker_sell_base_volume)
        *out->mutable_taker_sell_base_volume() = to_proto(*c.taker_sell_base_volume);
    out->set_is_closed(c.is_closed);
    out->set_source(static_cast<::alttech::v1::CandleSource>(static_cast<int>(c.source)));
    out->set_quality(static_cast<::alttech::v1::QualityStatus>(static_cast<int>(c.quality)));
    out->set_revision(c.revision);
    if (c.minute_count) out->set_minute_count(*c.minute_count);
}

Result<Candle> candle_from_proto(const ::alttech::v1::Candle& p) {
    Candle c;
    const int32_t tf = static_cast<int32_t>(p.timeframe());
    switch (tf) {
        case 1: c.timeframe = Timeframe::m1; break;
        case 5: c.timeframe = Timeframe::m5; break;
        case 15: c.timeframe = Timeframe::m15; break;
        case 30: c.timeframe = Timeframe::m30; break;
        case 60: c.timeframe = Timeframe::h1; break;
        case 240: c.timeframe = Timeframe::h4; break;
        case 1440: c.timeframe = Timeframe::d1; break;
        default:
            return fail(ErrorCode::validation_error, "unknown timeframe: " + std::to_string(tf));
    }
    c.open_time_ms = p.open_time_ms();
    c.close_time_ms = p.close_time_ms();
    c.open = from_proto(p.open());
    c.high = from_proto(p.high());
    c.low = from_proto(p.low());
    c.close = from_proto(p.close());
    c.base_volume = from_proto(p.base_volume());
    c.quote_volume = from_proto(p.quote_volume());
    if (p.has_trade_count()) c.trade_count = p.trade_count();
    if (p.has_taker_buy_base_volume()) c.taker_buy_base_volume = from_proto(p.taker_buy_base_volume());
    if (p.has_taker_sell_base_volume())
        c.taker_sell_base_volume = from_proto(p.taker_sell_base_volume());
    c.is_closed = p.is_closed();
    c.source = static_cast<CandleSource>(static_cast<int>(p.source()));
    c.quality = static_cast<Quality>(static_cast<int>(p.quality()));
    c.revision = p.revision();
    if (p.has_minute_count()) c.minute_count = p.minute_count();
    return c;
}

void to_proto(const Trade& t, ::alttech::v1::Trade* out) {
    out->set_exchange_trade_id(t.exchange_trade_id);
    *out->mutable_price() = to_proto(t.price);
    *out->mutable_size() = to_proto(t.size);
    out->set_side(t.side == TradeSide::buy    ? ::alttech::v1::TRADE_SIDE_BUY
                  : t.side == TradeSide::sell ? ::alttech::v1::TRADE_SIDE_SELL
                                              : ::alttech::v1::TRADE_SIDE_UNSPECIFIED);
    out->set_exchange_ts_ms(t.exchange_ts_ms);
}

Result<Trade> trade_from_proto(const ::alttech::v1::Trade& p) {
    Trade t;
    t.exchange_trade_id = p.exchange_trade_id();
    t.price = from_proto(p.price());
    t.size = from_proto(p.size());
    t.side = p.side() == ::alttech::v1::TRADE_SIDE_BUY    ? TradeSide::buy
             : p.side() == ::alttech::v1::TRADE_SIDE_SELL ? TradeSide::sell
                                                    : TradeSide::unspecified;
    t.exchange_ts_ms = p.exchange_ts_ms();
    return t;
}

::alttech::v1::Envelope make_envelope(const EnvelopeMeta& meta) {
    ::alttech::v1::Envelope env;
    env.set_schema_version(kSchemaVersion);
    env.set_message_id(meta.message_id);
    env.set_event_type(meta.event_type);
    // The venue the message is about, not a constant. See EnvelopeMeta::venue.
    env.set_exchange(static_cast<::alttech::v1::Exchange>(venue_id(meta.venue)));
    env.set_instrument_id(meta.instrument_id);
    env.set_exchange_symbol(meta.exchange_symbol);
    env.set_exchange_timestamp_ms(meta.exchange_timestamp_ms);
    env.set_produced_at_ms(timeutil::now_ms());
    env.set_trace_id(meta.trace_id);
    env.set_source_service(meta.source_service);
    return env;
}

std::string serialize(const ::alttech::v1::Envelope& env) {
    std::string out;
    env.SerializeToString(&out);
    return out;
}

Result<::alttech::v1::Envelope> parse_envelope(std::string_view bytes) {
    ::alttech::v1::Envelope env;
    if (!env.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) {
        return fail(ErrorCode::parse_error, "invalid protobuf envelope");
    }
    if (env.schema_version() != kSchemaVersion) {
        return fail(ErrorCode::validation_error,
                    "unsupported schema version: " + std::to_string(env.schema_version()));
    }
    return env;
}

std::string candle_msg_id(std::string_view exchange, std::string_view symbol, Timeframe tf,
                          int64_t open_time_ms) {
    std::string id("c:");
    id.append(exchange).append(":").append(symbol).append(":").append(tf_label(tf)).append(":");
    id.append(std::to_string(open_time_ms));
    return id;
}

} // namespace alttech::messaging
