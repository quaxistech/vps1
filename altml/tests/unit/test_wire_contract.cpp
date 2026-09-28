#include "alttech/feature.pb.h"
#include "alttech/system.pb.h"
#include "feature.pb.h"
#include "system.pb.h"

#include <catch2/catch_test_macros.hpp>

#include <google/protobuf/descriptor.h>

#include <string_view>

namespace {

const google::protobuf::FieldDescriptor* field(
    const google::protobuf::Descriptor& message,
    std::string_view name) {
    return message.FindFieldByName(std::string(name));
}

} // namespace

TEST_CASE("altml.v1 envelope keeps its published payload tags") {
    const auto& envelope = *altml::v1::Envelope::descriptor();
    const auto* schema_version = field(envelope, "schema_version");
    const auto* feature_vector = field(envelope, "feature_vector");
    const auto* prediction = field(envelope, "prediction");
    REQUIRE(schema_version != nullptr);
    REQUIRE(feature_vector != nullptr);
    REQUIRE(prediction != nullptr);
    CHECK(schema_version->number() == 1);
    CHECK(feature_vector->number() == 29);
    CHECK(prediction->number() == 31);
    CHECK(feature_vector->containing_oneof() != nullptr);
    CHECK(prediction->containing_oneof() != nullptr);
}

TEST_CASE("vendored AltTech feature view keeps the producer wire layout") {
    const auto& envelope = *alttech::v1::Envelope::descriptor();
    const auto* schema_version = field(envelope, "schema_version");
    const auto* feature_vector = field(envelope, "feature_vector");
    REQUIRE(schema_version != nullptr);
    REQUIRE(feature_vector != nullptr);
    CHECK(schema_version->number() == 1);
    CHECK(feature_vector->number() == 30);

    // The inference re-pack copies the inner FeatureVector bytes verbatim, so
    // the vendored producer message must mirror ours field-for-field.
    const auto& source = *alttech::v1::FeatureVector::descriptor();
    const auto& bridge = *altml::v1::FeatureVectorV1::descriptor();
    REQUIRE(source.field_count() == bridge.field_count());
    for (int index = 0; index < source.field_count(); ++index) {
        const auto* source_field = source.field(index);
        const auto* bridge_field = bridge.field(index);
        CAPTURE(source_field->name());
        CHECK(source_field->name() == bridge_field->name());
        CHECK(source_field->number() == bridge_field->number());
        CHECK(source_field->type() == bridge_field->type());
        CHECK(source_field->is_repeated() == bridge_field->is_repeated());
        CHECK(source_field->is_packed() == bridge_field->is_packed());
    }
}

// ------------------------------------------- prediction contract

// Pinned on the PRODUCER side too. AltStrategy vendors these field numbers and
// asserts them; if only the consumer pins them, the producer can still drift and
// the mismatch shows up as a parse that quietly yields nothing -- which is
// exactly how the previous contract divergence stayed invisible.
TEST_CASE("PredictionV1 keeps the field numbers AltStrategy vendors") {
    const auto* d = altml::v1::PredictionV1::descriptor();
    struct F { const char* name; int number; };
    for (const auto& f : {F{"prediction_id", 1}, F{"timeframe", 2}, F{"bar_open_time_ms", 3},
                          F{"bar_revision", 4}, F{"model_id", 5}, F{"model_version", 6},
                          F{"feature_set_version", 7}, F{"probability_up", 8},
                          F{"probability_down", 9}, F{"expected_return", 10},
                          F{"probability_target_before_stop", 11}, F{"expected_mfe", 12},
                          F{"expected_mae", 13}, F{"expected_holding_time_min", 14},
                          F{"confidence", 15}, F{"quality_status", 16},
                          F{"computed_at_ms", 17}, F{"latency_us", 18},
                          F{"prediction_schema_version", 19}, F{"target_venue", 20},
                          F{"abstain", 21},
                          // schema 4: market-prediction semantics
                          F{"probability_calibrated", 44}, F{"label_definition", 45},
                          F{"expected_up_excursion_bps", 46}, F{"expected_down_excursion_bps", 47},
                          F{"magnitude_source", 48}, F{"calibration_version", 49},
                          F{"model_role", 50}}) {
        const auto* fd = d->FindFieldByName(f.name);
        INFO("field " << f.name);
        REQUIRE(fd != nullptr);
        CHECK(fd->number() == f.number);
    }
}

TEST_CASE("the prediction carries no field AltML cannot compute") {
    // The consumer's old view expected these. None has a producer, and each was
    // read as a measured zero. They must not reappear on the wire.
    const auto* d = altml::v1::PredictionV1::descriptor();
    for (const char* absent : {"p_flat", "net_return_q25", "net_return_q50", "net_return_q75",
                               "mfe_q50", "mfe_q75", "mfe_q90", "mae_q50", "mae_q75", "mae_q90",
                               "volatility_q50", "ood_score", "uncertainty_score",
                               "quality_route"}) {
        INFO("field " << absent);
        CHECK(d->FindFieldByName(absent) == nullptr);
    }
}

TEST_CASE("the envelope carries the prediction at the tag the consumer expects") {
    const auto* d = altml::v1::Envelope::descriptor();
    const auto* fd = d->FindFieldByName("prediction");
    REQUIRE(fd != nullptr);
    CHECK(fd->number() == 31);
}
