#include "alttech/messaging/codec.hpp"

#include "system.pb.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace {

const google::protobuf::FieldDescriptor& field(
    const google::protobuf::Descriptor& message,
    std::string_view name) {
    const auto* result = message.FindFieldByName(std::string(name));
    REQUIRE(result != nullptr);
    return *result;
}

std::string decode_hex(std::string_view hex) {
    const auto nibble = [](char value) -> uint8_t {
        if (value >= '0' && value <= '9') return static_cast<uint8_t>(value - '0');
        if (value >= 'a' && value <= 'f') return static_cast<uint8_t>(value - 'a' + 10);
        return static_cast<uint8_t>(value - 'A' + 10);
    };
    REQUIRE(hex.size() % 2 == 0);
    std::string bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t index = 0; index < hex.size(); index += 2) {
        bytes.push_back(static_cast<char>((nibble(hex[index]) << 4U) | nibble(hex[index + 1])));
    }
    return bytes;
}

} // namespace

TEST_CASE("AltTech owns its protobuf API while preserving AltSpotter input tags") {
    const auto& envelope = *::alttech::v1::Envelope::descriptor();
    CHECK(envelope.full_name() == "alttech.v1.Envelope");
    CHECK(field(envelope, "trade").number() == 20);
    CHECK(field(envelope, "candle").number() == 21);
    CHECK(field(envelope, "candle_batch").number() == 22);
    CHECK(field(envelope, "trade").message_type()->full_name() == "alttech.v1.Trade");
    CHECK(field(envelope, "candle").message_type()->full_name() == "alttech.v1.Candle");
    CHECK(field(envelope, "candle_batch").message_type()->full_name() ==
          "alttech.v1.CandleBatch");

    const auto& trade = *::alttech::v1::Trade::descriptor();
    CHECK(field(trade, "exchange_trade_id").number() == 1);
    CHECK(field(trade, "price").number() == 2);
    CHECK(field(trade, "size").number() == 3);
    CHECK(field(trade, "side").number() == 4);
    CHECK(field(trade, "exchange_ts_ms").number() == 5);

    const auto& candle = *::alttech::v1::Candle::descriptor();
    const std::array<std::string_view, 17> candle_fields = {
        "timeframe",          "open_time_ms",          "close_time_ms",
        "open",               "high",                  "low",
        "close",              "base_volume",           "quote_volume",
        "trade_count",        "taker_buy_base_volume", "taker_sell_base_volume",
        "is_closed",          "source",                "quality",
        "revision",           "minute_count"};
    for (size_t index = 0; index < candle_fields.size(); ++index) {
        CHECK(field(candle, candle_fields[index]).number() == static_cast<int>(index + 1));
    }
}

TEST_CASE("AltTech parser accepts package-agnostic AltSpotter envelope wire tags") {
    const std::array<std::pair<std::string, ::alttech::v1::Envelope::PayloadCase>, 3> fixtures = {{
        {std::string{"\x08\x01\xa2\x01\x00", 5}, ::alttech::v1::Envelope::kTrade},
        {std::string{"\x08\x01\xaa\x01\x00", 5}, ::alttech::v1::Envelope::kCandle},
        {std::string{"\x08\x01\xb2\x01\x00", 5}, ::alttech::v1::Envelope::kCandleBatch},
    }};

    for (const auto& [bytes, expected] : fixtures) {
        const auto parsed = alttech::messaging::parse_envelope(bytes);
        REQUIRE(parsed);
        CHECK(parsed->payload_case() == expected);
    }

    // Captured from AltSpotter's own schema encoder. Keeping this non-empty
    // fixture prevents a local package rename from hiding a field-level drift.
    const std::string trade_fixture = decode_hex(
        "08011210616c7473706f747465722d74726164651a0574726164652001282a3207"
        "4254435553445438fbe0f4bf873240c8e3f4bf8732520a616c7473706f747465"
        "72a2011e0a043930303112060884f85110011a0508fa011003200128fbe0f4bf"
        "8732");
    const auto parsed = alttech::messaging::parse_envelope(trade_fixture);
    REQUIRE(parsed);
    CHECK(parsed->instrument_id() == 42);
    CHECK(parsed->exchange_symbol() == "BTCUSDT");
    REQUIRE(parsed->has_trade());
    CHECK(parsed->trade().exchange_trade_id() == "9001");
    CHECK(parsed->trade().price().mantissa() == 671234);
    CHECK(parsed->trade().price().scale() == 1);
    CHECK(parsed->trade().size().mantissa() == 125);
    CHECK(parsed->trade().size().scale() == 3);
    CHECK(parsed->trade().side() == ::alttech::v1::TRADE_SIDE_BUY);
    CHECK(parsed->trade().exchange_ts_ms() == 1720000000123);
}

TEST_CASE("AltTech output envelope exposes only typed analytics payloads") {
    const auto& envelope = *::alttech::v1::Envelope::descriptor();
    CHECK(field(envelope, "feature_vector").number() == 30);
    CHECK(field(envelope, "microstructure_vector").number() == 31);
    CHECK(field(envelope, "structure_event").number() == 32);
    CHECK(field(envelope, "regime_snapshot").number() == 33);
    CHECK(field(envelope, "combined_regime").number() == 34);
    CHECK(field(envelope, "status_event").number() == 35);
    CHECK(envelope.FindFieldByName("integrity_event") == nullptr);
    CHECK(envelope.FindFieldByName("instrument_event") == nullptr);
    CHECK(envelope.FindFieldByName("prediction") == nullptr);
}

// Moved here from the topology test: the envelope check needs the generated
// protobuf, which only this target links.
TEST_CASE("the envelope carries the venue the message is about") {
    using alttech::Venue;
    for (const Venue v : alttech::kAllVenues) {
        alttech::messaging::EnvelopeMeta meta;
        meta.event_type = "feature.core";
        meta.venue = v;
        meta.instrument_id = 1;
        meta.exchange_symbol = "BTCUSDT";
        const auto env = alttech::messaging::make_envelope(meta);
        CHECK(static_cast<int>(env.exchange()) == alttech::venue_id(v));
    }
    // The default remains the primary venue, so the genuinely Bitget-only
    // publishers keep their existing meaning.
    alttech::messaging::EnvelopeMeta def;
    def.event_type = "status.feature.ready";
    CHECK(static_cast<int>(alttech::messaging::make_envelope(def).exchange()) ==
          alttech::venue_id(alttech::kPrimaryVenue));
}
