#include "messaging/codec.hpp"

#include "core/time.hpp"

namespace altspotter::messaging {

altspotter::v1::Decimal to_proto(const Decimal& d) {
    altspotter::v1::Decimal out;
    out.set_mantissa(d.mantissa());
    out.set_scale(d.scale());
    return out;
}

Decimal from_proto(const altspotter::v1::Decimal& d) {
    return Decimal{d.mantissa(), d.scale()};
}

void to_proto(const Candle& c, altspotter::v1::Candle* out) {
    out->set_timeframe(static_cast<altspotter::v1::Timeframe>(tf_minutes(c.timeframe)));
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
    out->set_source(static_cast<altspotter::v1::CandleSource>(static_cast<int>(c.source)));
    out->set_quality(static_cast<altspotter::v1::QualityStatus>(static_cast<int>(c.quality)));
    out->set_revision(c.revision);
    if (c.minute_count) out->set_minute_count(*c.minute_count);
}

Result<Candle> candle_from_proto(const altspotter::v1::Candle& p) {
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

void to_proto(const Trade& t, altspotter::v1::Trade* out) {
    out->set_exchange_trade_id(t.exchange_trade_id);
    *out->mutable_price() = to_proto(t.price);
    *out->mutable_size() = to_proto(t.size);
    out->set_side(t.side == TradeSide::buy    ? altspotter::v1::TRADE_SIDE_BUY
                  : t.side == TradeSide::sell ? altspotter::v1::TRADE_SIDE_SELL
                                              : altspotter::v1::TRADE_SIDE_UNSPECIFIED);
    out->set_exchange_ts_ms(t.exchange_ts_ms);
}

Result<Trade> trade_from_proto(const altspotter::v1::Trade& p) {
    Trade t;
    t.exchange_trade_id = p.exchange_trade_id();
    t.price = from_proto(p.price());
    t.size = from_proto(p.size());
    t.side = p.side() == altspotter::v1::TRADE_SIDE_BUY    ? TradeSide::buy
             : p.side() == altspotter::v1::TRADE_SIDE_SELL ? TradeSide::sell
                                                    : TradeSide::unspecified;
    t.exchange_ts_ms = p.exchange_ts_ms();
    return t;
}

void to_proto(const InstrumentSpec& s, int64_t instrument_id, const std::string& normalized_hash,
              int64_t spec_version, altspotter::v1::InstrumentSpec* out, int16_t exchange_id) {
    out->set_exchange(static_cast<altspotter::v1::Exchange>(exchange_id));
    out->set_instrument_id(instrument_id);
    out->set_exchange_symbol(s.exchange_symbol);
    out->set_base_asset(s.base_asset);
    out->set_quote_asset(s.quote_asset);
    out->set_contract_type(s.contract_type);
    out->set_status(static_cast<altspotter::v1::InstrumentStatus>(static_cast<int>(s.status)));
    out->set_list_time_ms(s.list_time_ms);
    *out->mutable_tick_size() = to_proto(s.tick_size);
    out->set_price_precision(s.price_precision);
    *out->mutable_quantity_step() = to_proto(s.quantity_step);
    out->set_quantity_precision(s.quantity_precision);
    *out->mutable_min_order_quantity() = to_proto(s.min_order_quantity);
    *out->mutable_min_notional() = to_proto(s.min_notional);
    *out->mutable_maker_fee_rate() = to_proto(s.maker_fee_rate);
    *out->mutable_taker_fee_rate() = to_proto(s.taker_fee_rate);
    out->set_min_leverage(s.min_leverage);
    out->set_max_leverage(s.max_leverage);
    out->set_funding_interval_hours(s.funding_interval_hours);
    out->set_normalized_hash(normalized_hash);
    out->set_spec_version(spec_version);
    out->set_settle_asset(s.settle_asset);
    *out->mutable_contract_value() = to_proto(s.contract_value);
    out->set_market_type(altspotter::v1::MARKET_TYPE_USDT_PERP);
}

altspotter::v1::Envelope make_envelope(const EnvelopeMeta& meta) {
    altspotter::v1::Envelope env;
    env.set_schema_version(kSchemaVersion);
    env.set_message_id(meta.message_id);
    env.set_event_type(meta.event_type);
    env.set_exchange(static_cast<altspotter::v1::Exchange>(meta.exchange_id));
    env.set_instrument_id(meta.instrument_id);
    env.set_exchange_symbol(meta.exchange_symbol);
    env.set_exchange_timestamp_ms(meta.exchange_timestamp_ms);
    env.set_produced_at_ms(timeutil::now_ms());
    env.set_trace_id(meta.trace_id);
    env.set_source_service(meta.source_service);
    env.set_market_type(altspotter::v1::MARKET_TYPE_USDT_PERP);
    if (meta.instrument_id > 0) env.set_canonical_pair_id(meta.instrument_id);
    if (!meta.connection_id.empty()) env.set_connection_id(meta.connection_id);
    if (!meta.source_channel.empty()) env.set_source_channel(meta.source_channel);
    if (meta.sequence != 0) env.set_sequence(meta.sequence);
    if (meta.local_receive_ms != 0) env.set_local_receive_time_ms(meta.local_receive_ms);
    return env;
}

std::string serialize(const altspotter::v1::Envelope& env) {
    std::string out;
    env.SerializeToString(&out);
    return out;
}

Result<altspotter::v1::Envelope> parse_envelope(std::string_view bytes) {
    altspotter::v1::Envelope env;
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

} // namespace altspotter::messaging
