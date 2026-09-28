// Explicit, create-only JetStream topology management for AltTech.
//
// Default/--check mode is read-only. --apply may create the AltTech-owned
// output stream and durable bar consumer, but it never updates or deletes any
// resource and never mutates the externally owned input stream.

#include "alttech/logging/logger.hpp"
#include "alttech/messaging/topology.hpp"

#include <nats.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

using alttech::log::error;
using alttech::log::info;

namespace {

enum class Mode { check, apply, recreate_consumer, reconcile_limits };

struct Options {
    Mode mode = Mode::check;
    std::string url;
    std::string input_stream =
        std::string(alttech::messaging::topology::kDefaultInputStream);
    std::string output_stream =
        std::string(alttech::messaging::topology::kDefaultOutputStream);
    std::string durable =
        std::string(alttech::messaging::topology::kDefaultBarConsumer);
};

constexpr std::string_view kOutputDescription =
    "AltTech analytics output; create-only";
constexpr std::string_view kConsumerDescription =
    "AltTech bar input; create-only";
constexpr int64_t kNanosecondsPerSecond = 1'000'000'000LL;
// The analytics stream is a REPLAY window for live consumers, not an archive:
// AltML and AltStrategy read a vector within seconds of publication, and the
// durable copy lives in PostgreSQL. Two days covers the trading horizon and any
// consumer restart. The previous 30 days pinned the full 3 GiB cap of
// JetStream store on disk for messages nothing would read again.
constexpr int64_t kOutputMaxBytes = 1LL * 1024 * 1024 * 1024;
constexpr int64_t kOutputMaxAge = 2LL * 24 * 60 * 60 * kNanosecondsPerSecond;
constexpr int32_t kOutputMaxMessageSize = 1024 * 1024;
constexpr int64_t kDuplicateWindow = 2LL * 60 * kNanosecondsPerSecond;
constexpr int64_t kConsumerAckWait = 120LL * kNanosecondsPerSecond;
constexpr int64_t kConsumerMaxDeliver = -1;
constexpr int64_t kConsumerMaxWaiting = 512;
constexpr int64_t kConsumerMaxAckPending = 2048;
constexpr int64_t kConsumerMaxRequestBatch = 256;
constexpr int64_t kConsumerMaxRequestExpires = 5LL * kNanosecondsPerSecond;

struct StreamInfoDeleter {
    void operator()(jsStreamInfo* value) const noexcept {
        if (value != nullptr) jsStreamInfo_Destroy(value);
    }
};

struct ConsumerInfoDeleter {
    void operator()(jsConsumerInfo* value) const noexcept {
        if (value != nullptr) jsConsumerInfo_Destroy(value);
    }
};

using StreamInfoPtr = std::unique_ptr<jsStreamInfo, StreamInfoDeleter>;
using ConsumerInfoPtr = std::unique_ptr<jsConsumerInfo, ConsumerInfoDeleter>;

struct StreamLookup {
    natsStatus status = NATS_ERR;
    jsErrCode jetstream_error = static_cast<jsErrCode>(0);
    StreamInfoPtr info;
};

struct ConsumerLookup {
    natsStatus status = NATS_ERR;
    jsErrCode jetstream_error = static_cast<jsErrCode>(0);
    ConsumerInfoPtr info;
};

class Session {
public:
    Session() = default;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    ~Session() {
        if (jetstream_ != nullptr) jsCtx_Destroy(jetstream_);
        if (connection_ != nullptr) {
            natsConnection_Close(connection_);
            natsConnection_Destroy(connection_);
        }
        if (options_ != nullptr) natsOptions_Destroy(options_);
    }

    bool connect(const std::string& url) {
        natsStatus status = natsOptions_Create(&options_);
        if (status == NATS_OK) status = natsOptions_SetURL(options_, url.c_str());
        if (status == NATS_OK) status = natsOptions_SetName(options_, "alttech-nats-setup");
        if (status == NATS_OK) status = natsOptions_SetTimeout(options_, 5000);
        if (status == NATS_OK) status = natsConnection_Connect(&connection_, options_);
        if (status == NATS_OK) {
            status = natsConnection_JetStream(&jetstream_, connection_, nullptr);
        }
        if (status != NATS_OK) {
            error("nats_setup_connect_failed",
                  {{"error_code", natsStatus_GetText(status)}});
            return false;
        }
        return true;
    }

    [[nodiscard]] jsCtx* jetstream() const noexcept { return jetstream_; }

private:
    natsOptions* options_ = nullptr;
    natsConnection* connection_ = nullptr;
    jsCtx* jetstream_ = nullptr;
};

std::string environment_or(std::string_view name, std::string fallback) {
    if (const char* value = std::getenv(std::string(name).c_str()); value != nullptr) {
        return value;
    }
    return fallback;
}

std::string nullable_string(const char* value) {
    return value == nullptr ? std::string{} : std::string(value);
}

void print_usage(std::ostream& output) {
    output
        << "usage: alttech-nats-setup [--check|--apply|--reconcile-limits|\n"
           "                            --recreate-consumer]\n"
           "                           [--url NATS_URL]\n"
           "                           [--input-stream NAME] [--output-stream NAME]\n"
           "                           [--durable NAME]\n"
           "\n"
           "\n"
           "Default mode is read-only --check. --apply is create-only: it never\n"
           "updates and never deletes. --reconcile-limits moves max_bytes and\n"
           "max_age on AltTech's OWN output stream to this binary's contract,\n"
           "which create-only provisioning cannot express; it changes nothing\n"
           "else. --recreate-consumer deletes and recreates\n"
           "AltTech's OWN durable on the input stream, which is the supported way\n"
           "to adopt a changed filter subject. It touches nothing else, and the\n"
           "durable's position is not history: PostgreSQL is.\n"
           "Environment: ALTTECH_NATS_URL, ALTTECH_NATS_INPUT_STREAM,\n"
           "             ALTTECH_NATS_OUTPUT_STREAM, ALTTECH_NATS_DURABLE.\n";
}

bool parse_options(Options& options, int argc, char** argv) {
    options.url = environment_or("ALTTECH_NATS_URL", {});
    options.input_stream = environment_or("ALTTECH_NATS_INPUT_STREAM",
                                          std::move(options.input_stream));
    options.output_stream = environment_or("ALTTECH_NATS_OUTPUT_STREAM",
                                           std::move(options.output_stream));
    options.durable = environment_or("ALTTECH_NATS_DURABLE", std::move(options.durable));

    bool explicit_check = false;
    bool explicit_apply = false;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        const auto read_value = [&](std::string& destination) -> bool {
            if (index + 1 >= argc) return false;
            destination = argv[++index];
            return true;
        };

        if (argument == "--check") {
            explicit_check = true;
            options.mode = Mode::check;
        } else if (argument == "--apply") {
            explicit_apply = true;
            options.mode = Mode::apply;
        } else if (argument == "--reconcile-limits") {
            options.mode = Mode::reconcile_limits;
        } else if (argument == "--recreate-consumer") {
            explicit_apply = true;
            options.mode = Mode::recreate_consumer;
        } else if (argument == "--url") {
            if (!read_value(options.url)) return false;
        } else if (argument == "--input-stream") {
            if (!read_value(options.input_stream)) return false;
        } else if (argument == "--output-stream") {
            if (!read_value(options.output_stream)) return false;
        } else if (argument == "--durable") {
            if (!read_value(options.durable)) return false;
        } else if (argument == "--help" || argument == "-h") {
            print_usage(std::cout);
            std::exit(0);
        } else {
            return false;
        }
    }

    if (explicit_check && explicit_apply) return false;
    return true;
}

bool validate_options(const Options& options) {
    if (options.url.empty()) {
        std::cerr << "missing NATS URL: use --url or ALTTECH_NATS_URL\n";
        return false;
    }
    if (options.input_stream == options.output_stream) {
        std::cerr << "input and output streams must be different resources\n";
        return false;
    }
    const auto validate_name = [](std::string_view flag, std::string_view value) {
        if (alttech::messaging::topology::valid_resource_name(value)) return true;
        std::cerr << flag << " must contain only ASCII letters, digits, '_' or '-'"
                  << " and be at most 255 characters\n";
        return false;
    };
    return validate_name("--input-stream", options.input_stream) &&
           validate_name("--output-stream", options.output_stream) &&
           validate_name("--durable", options.durable);
}

StreamLookup lookup_stream(jsCtx* jetstream, const std::string& name) {
    jsStreamInfo* raw = nullptr;
    jsErrCode jetstream_error = static_cast<jsErrCode>(0);
    const natsStatus status =
        js_GetStreamInfo(&raw, jetstream, name.c_str(), nullptr, &jetstream_error);
    return StreamLookup{status, jetstream_error, StreamInfoPtr(raw)};
}

ConsumerLookup lookup_consumer(jsCtx* jetstream, const std::string& stream,
                               const std::string& durable) {
    jsConsumerInfo* raw = nullptr;
    jsErrCode jetstream_error = static_cast<jsErrCode>(0);
    const natsStatus status = js_GetConsumerInfo(&raw, jetstream, stream.c_str(),
                                                 durable.c_str(), nullptr,
                                                 &jetstream_error);
    return ConsumerLookup{status, jetstream_error, ConsumerInfoPtr(raw)};
}

bool has_subject(const jsStreamConfig& config, std::string_view expected) {
    for (int index = 0; index < config.SubjectsLen; ++index) {
        if (config.Subjects[index] != nullptr && config.Subjects[index] == expected) {
            return true;
        }
    }
    return false;
}

// An externally owned stream only has to *cover* the subject AltTech reads; it
// is configured for its owner's needs, not for ours.
bool covers_subject(const jsStreamConfig& config, std::string_view required) {
    for (int index = 0; index < config.SubjectsLen; ++index) {
        if (config.Subjects[index] != nullptr &&
            alttech::messaging::topology::subject_covers(config.Subjects[index], required)) {
            return true;
        }
    }
    return false;
}

bool has_metadata(const natsMetadata& metadata, std::string_view key,
                  std::string_view value) {
    if (metadata.List == nullptr) return false;
    for (int index = 0; index < metadata.Count; ++index) {
        const char* actual_key = metadata.List[index * 2];
        const char* actual_value = metadata.List[index * 2 + 1];
        if (actual_key != nullptr && actual_value != nullptr && actual_key == key &&
            actual_value == value) {
            return true;
        }
    }
    return false;
}

template <typename Expected, typename Actual>
bool require_equal(bool condition, std::string_view resource, std::string_view field,
                   Expected expected, Actual actual) {
    if (condition) return true;
    error("nats_topology_drift",
          {{"resource", resource},
           {"field", field},
           {"expected", expected},
           {"actual", actual}});
    return false;
}

bool validate_input_stream(const Options& options, const StreamLookup& lookup) {
    if (lookup.status == NATS_NOT_FOUND) {
        error("nats_input_stream_missing", {{"stream", options.input_stream}});
        return false;
    }
    if (lookup.status != NATS_OK || lookup.info == nullptr ||
        lookup.info->Config == nullptr) {
        error("nats_input_stream_lookup_failed",
              {{"stream", options.input_stream},
               {"error_code", natsStatus_GetText(lookup.status)},
               {"jetstream_code", static_cast<int>(lookup.jetstream_error)}});
        return false;
    }
    if (!covers_subject(*lookup.info->Config,
                        alttech::messaging::topology::kBarSubject)) {
        error("nats_input_stream_incompatible",
              {{"stream", options.input_stream},
               {"required_subject", alttech::messaging::topology::kBarSubject}});
        return false;
    }
    info("nats_input_stream_validated",
         {{"stream", options.input_stream},
          {"required_subject", alttech::messaging::topology::kBarSubject},
          {"ownership", "external_read_only"}});
    return true;
}

bool validate_output_stream(const Options& options, const jsStreamConfig& actual) {
    bool valid = true;
    const std::string resource = "stream/" + options.output_stream;
    valid = require_equal(actual.SubjectsLen == 1 &&
                              has_subject(actual,
                                          alttech::messaging::topology::kAnalyticsSubject),
                          resource, "subjects",
                          std::string(alttech::messaging::topology::kAnalyticsSubject),
                          actual.SubjectsLen) &&
            valid;
    valid = require_equal(nullable_string(actual.Description) == kOutputDescription,
                          resource, "description", std::string(kOutputDescription),
                          nullable_string(actual.Description)) &&
            valid;
    valid = require_equal(actual.Retention == js_LimitsPolicy, resource, "retention",
                          static_cast<int>(js_LimitsPolicy),
                          static_cast<int>(actual.Retention)) &&
            valid;
    valid = require_equal(actual.Storage == js_FileStorage, resource, "storage",
                          static_cast<int>(js_FileStorage),
                          static_cast<int>(actual.Storage)) &&
            valid;
    valid = require_equal(actual.Replicas == 1, resource, "replicas", 1,
                          actual.Replicas) &&
            valid;
    valid = require_equal(actual.Discard == js_DiscardOld, resource, "discard",
                          static_cast<int>(js_DiscardOld),
                          static_cast<int>(actual.Discard)) &&
            valid;
    valid = require_equal(actual.MaxConsumers == -1, resource, "max_consumers", -1,
                          actual.MaxConsumers) &&
            valid;
    valid = require_equal(actual.MaxMsgs == -1, resource, "max_messages", -1,
                          actual.MaxMsgs) &&
            valid;
    valid = require_equal(actual.MaxBytes == kOutputMaxBytes, resource, "max_bytes",
                          kOutputMaxBytes, actual.MaxBytes) &&
            valid;
    valid = require_equal(actual.MaxAge == kOutputMaxAge, resource, "max_age_ns",
                          kOutputMaxAge, actual.MaxAge) &&
            valid;
    valid = require_equal(actual.MaxMsgsPerSubject == -1, resource,
                          "max_messages_per_subject", -1,
                          actual.MaxMsgsPerSubject) &&
            valid;
    valid = require_equal(actual.MaxMsgSize == kOutputMaxMessageSize, resource,
                          "max_message_size", kOutputMaxMessageSize,
                          actual.MaxMsgSize) &&
            valid;
    valid = require_equal(actual.Duplicates == kDuplicateWindow, resource,
                          "duplicate_window_ns", kDuplicateWindow,
                          actual.Duplicates) &&
            valid;
    valid = require_equal(!actual.DenyDelete, resource, "deny_message_delete", false,
                          actual.DenyDelete) &&
            valid;
    valid = require_equal(!actual.DenyPurge, resource, "deny_purge", false,
                          actual.DenyPurge) &&
            valid;
    valid = require_equal(actual.Mirror == nullptr && actual.SourcesLen == 0,
                          resource, "mirror_or_sources", "none",
                          actual.Mirror != nullptr || actual.SourcesLen != 0 ? "present"
                                                                          : "none") &&
            valid;
    valid = require_equal(has_metadata(actual.Metadata, "owner", "alttech"),
                          resource, "metadata.owner", "alttech", "missing_or_other") &&
            valid;
    valid = require_equal(has_metadata(actual.Metadata, "topology", "v1"), resource,
                          "metadata.topology", "v1", "missing_or_other") &&
            valid;
    return valid;
}

bool validate_consumer(const Options& options, const jsConsumerConfig& actual) {
    bool valid = true;
    const std::string resource =
        "consumer/" + options.input_stream + "/" + options.durable;
    valid = require_equal(nullable_string(actual.Durable) == options.durable, resource,
                          "durable", options.durable,
                          nullable_string(actual.Durable)) &&
            valid;
    valid = require_equal(nullable_string(actual.Description) == kConsumerDescription,
                          resource, "description", std::string(kConsumerDescription),
                          nullable_string(actual.Description)) &&
            valid;
    valid = require_equal(actual.DeliverSubject == nullptr ||
                              *actual.DeliverSubject == '\0',
                          resource, "delivery", "pull", "push") &&
            valid;
    valid = require_equal(actual.DeliverPolicy == js_DeliverNew, resource,
                          "deliver_policy", static_cast<int>(js_DeliverNew),
                          static_cast<int>(actual.DeliverPolicy)) &&
            valid;
    valid = require_equal(actual.AckPolicy == js_AckExplicit, resource, "ack_policy",
                          static_cast<int>(js_AckExplicit),
                          static_cast<int>(actual.AckPolicy)) &&
            valid;
    valid = require_equal(actual.AckWait == kConsumerAckWait, resource, "ack_wait_ns",
                          kConsumerAckWait, actual.AckWait) &&
            valid;
    valid = require_equal(actual.MaxDeliver == kConsumerMaxDeliver, resource,
                          "max_deliver", kConsumerMaxDeliver, actual.MaxDeliver) &&
            valid;
    valid = require_equal(nullable_string(actual.FilterSubject) ==
                              alttech::messaging::topology::kBarSubject,
                          resource, "filter_subject",
                          std::string(alttech::messaging::topology::kBarSubject),
                          nullable_string(actual.FilterSubject)) &&
            valid;
    valid = require_equal(actual.ReplayPolicy == js_ReplayInstant, resource,
                          "replay_policy", static_cast<int>(js_ReplayInstant),
                          static_cast<int>(actual.ReplayPolicy)) &&
            valid;
    valid = require_equal(actual.MaxWaiting == kConsumerMaxWaiting, resource,
                          "max_waiting", kConsumerMaxWaiting, actual.MaxWaiting) &&
            valid;
    valid = require_equal(actual.MaxAckPending == kConsumerMaxAckPending, resource,
                          "max_ack_pending", kConsumerMaxAckPending,
                          actual.MaxAckPending) &&
            valid;
    valid = require_equal(actual.MaxRequestBatch == kConsumerMaxRequestBatch, resource,
                          "max_request_batch", kConsumerMaxRequestBatch,
                          actual.MaxRequestBatch) &&
            valid;
    valid = require_equal(actual.MaxRequestExpires == kConsumerMaxRequestExpires,
                          resource, "max_request_expires_ns",
                          kConsumerMaxRequestExpires, actual.MaxRequestExpires) &&
            valid;
    valid = require_equal(has_metadata(actual.Metadata, "owner", "alttech"),
                          resource, "metadata.owner", "alttech", "missing_or_other") &&
            valid;
    valid = require_equal(has_metadata(actual.Metadata, "topology", "v1"), resource,
                          "metadata.topology", "v1", "missing_or_other") &&
            valid;
    return valid;
}

// Bring the retention limits of AltTech's OWN output stream in line with this
// binary's contract. Create-only provisioning cannot express a limits change, so
// without this a retention decision could never be rolled out: the stream keeps
// the old window forever and --check then refuses to let the service start.
//
// Only max_bytes / max_age move, and only on the stream AltTech owns. Subjects,
// storage, retention policy and every other consumer are untouched, and no
// message is deleted beyond what the new window itself expires — the stream is a
// replay buffer, PostgreSQL is the history.
bool reconcile_output_limits(jsCtx* jetstream, const Options& options,
                             const jsStreamConfig& current) {
    jsStreamConfig config = current;
    config.MaxBytes = kOutputMaxBytes;
    config.MaxAge = kOutputMaxAge;

    jsStreamInfo* raw = nullptr;
    jsErrCode jetstream_error = static_cast<jsErrCode>(0);
    const natsStatus status =
        js_UpdateStream(&raw, jetstream, &config, nullptr, &jetstream_error);
    StreamInfoPtr updated(raw);
    if (status != NATS_OK) {
        error("nats_output_stream_limits_update_failed",
              {{"stream", options.output_stream},
               {"error_code", natsStatus_GetText(status)},
               {"jetstream_code", static_cast<int>(jetstream_error)}});
        return false;
    }
    info("nats_output_stream_limits_reconciled",
         {{"stream", options.output_stream},
          {"max_bytes", kOutputMaxBytes},
          {"max_age_ns", kOutputMaxAge}});
    return true;
}

bool ensure_output_stream(jsCtx* jetstream, const Options& options) {
    auto existing = lookup_stream(jetstream, options.output_stream);
    if (existing.status == NATS_OK && existing.info != nullptr &&
        existing.info->Config != nullptr) {
        if (options.mode == Mode::reconcile_limits) {
            return reconcile_output_limits(jetstream, options, *existing.info->Config);
        }
        const bool valid = validate_output_stream(options, *existing.info->Config);
        if (valid) {
            info("nats_output_stream_validated",
                 {{"stream", options.output_stream}, {"ownership", "alttech"}});
        }
        return valid;
    }
    if (existing.status != NATS_NOT_FOUND) {
        error("nats_output_stream_lookup_failed",
              {{"stream", options.output_stream},
               {"error_code", natsStatus_GetText(existing.status)},
               {"jetstream_code", static_cast<int>(existing.jetstream_error)}});
        return false;
    }
    if (options.mode == Mode::check) {
        error("nats_output_stream_missing", {{"stream", options.output_stream}});
        return false;
    }

    jsStreamConfig config;
    if (jsStreamConfig_Init(&config) != NATS_OK) {
        error("nats_output_stream_config_failed", {{"stream", options.output_stream}});
        return false;
    }
    const char* subjects[] = {alttech::messaging::topology::kAnalyticsSubject.data()};
    const char* metadata[] = {"owner", "alttech", "topology", "v1"};
    config.Name = options.output_stream.c_str();
    config.Description = kOutputDescription.data();
    config.Subjects = subjects;
    config.SubjectsLen = 1;
    config.Retention = js_LimitsPolicy;
    config.MaxConsumers = -1;
    config.MaxMsgs = -1;
    config.MaxBytes = kOutputMaxBytes;
    config.MaxAge = kOutputMaxAge;
    config.MaxMsgsPerSubject = -1;
    config.MaxMsgSize = kOutputMaxMessageSize;
    config.Discard = js_DiscardOld;
    config.Storage = js_FileStorage;
    config.Replicas = 1;
    config.Duplicates = kDuplicateWindow;
    config.DenyDelete = false;
    config.DenyPurge = false;
    config.Metadata.List = metadata;
    config.Metadata.Count = 2;

    jsStreamInfo* raw = nullptr;
    jsErrCode jetstream_error = static_cast<jsErrCode>(0);
    const natsStatus status =
        js_AddStream(&raw, jetstream, &config, nullptr, &jetstream_error);
    StreamInfoPtr created(raw);
    if (status == NATS_OK && created != nullptr && created->Config != nullptr) {
        if (!validate_output_stream(options, *created->Config)) return false;
        info("nats_output_stream_created",
             {{"stream", options.output_stream},
              {"subject", alttech::messaging::topology::kAnalyticsSubject}});
        return true;
    }

    // A concurrent create is safe: re-read and accept only an exact topology.
    auto raced = lookup_stream(jetstream, options.output_stream);
    if (raced.status == NATS_OK && raced.info != nullptr &&
        raced.info->Config != nullptr) {
        return validate_output_stream(options, *raced.info->Config);
    }
    error("nats_output_stream_create_failed",
          {{"stream", options.output_stream},
           {"error_code", natsStatus_GetText(status)},
           {"jetstream_code", static_cast<int>(jetstream_error)}});
    return false;
}

bool ensure_consumer(jsCtx* jetstream, const Options& options) {
    auto existing = lookup_consumer(jetstream, options.input_stream, options.durable);
    if (options.mode == Mode::recreate_consumer && existing.status == NATS_OK) {
        // Only ever AltTech's own durable, never the stream and never another
        // consumer on it. Its position is not history — PostgreSQL is — so
        // recreating it costs nothing but the messages already superseded there.
        jsErrCode delete_error = static_cast<jsErrCode>(0);
        const natsStatus deleted = js_DeleteConsumer(jetstream, options.input_stream.c_str(),
                                                     options.durable.c_str(), nullptr,
                                                     &delete_error);
        if (deleted != NATS_OK) {
            error("nats_bar_consumer_delete_failed",
                  {{"stream", options.input_stream},
                   {"durable", options.durable},
                   {"error_code", natsStatus_GetText(deleted)},
                   {"jetstream_code", static_cast<int>(delete_error)}});
            return false;
        }
        info("nats_bar_consumer_deleted",
             {{"stream", options.input_stream}, {"durable", options.durable}});
        existing = lookup_consumer(jetstream, options.input_stream, options.durable);
    }
    if (existing.status == NATS_OK && existing.info != nullptr &&
        existing.info->Config != nullptr) {
        const bool valid = validate_consumer(options, *existing.info->Config);
        if (valid) {
            info("nats_bar_consumer_validated",
                 {{"stream", options.input_stream},
                  {"durable", options.durable},
                  {"ownership", "alttech"}});
        }
        return valid;
    }
    if (existing.status != NATS_NOT_FOUND) {
        error("nats_bar_consumer_lookup_failed",
              {{"stream", options.input_stream},
               {"durable", options.durable},
               {"error_code", natsStatus_GetText(existing.status)},
               {"jetstream_code", static_cast<int>(existing.jetstream_error)}});
        return false;
    }
    if (options.mode == Mode::check) {
        error("nats_bar_consumer_missing",
              {{"stream", options.input_stream}, {"durable", options.durable}});
        return false;
    }

    jsConsumerConfig config;
    if (jsConsumerConfig_Init(&config) != NATS_OK) {
        error("nats_bar_consumer_config_failed", {{"durable", options.durable}});
        return false;
    }
    const char* metadata[] = {"owner", "alttech", "topology", "v1"};
    config.Durable = options.durable.c_str();
    config.Description = kConsumerDescription.data();
    config.DeliverPolicy = js_DeliverNew;
    config.AckPolicy = js_AckExplicit;
    config.AckWait = kConsumerAckWait;
    config.MaxDeliver = kConsumerMaxDeliver;
    config.FilterSubject = alttech::messaging::topology::kBarSubject.data();
    config.ReplayPolicy = js_ReplayInstant;
    config.MaxWaiting = kConsumerMaxWaiting;
    config.MaxAckPending = kConsumerMaxAckPending;
    config.MaxRequestBatch = kConsumerMaxRequestBatch;
    config.MaxRequestExpires = kConsumerMaxRequestExpires;
    config.Metadata.List = metadata;
    config.Metadata.Count = 2;

    jsConsumerInfo* raw = nullptr;
    jsErrCode jetstream_error = static_cast<jsErrCode>(0);
    const natsStatus status =
        js_AddConsumer(&raw, jetstream, options.input_stream.c_str(), &config, nullptr,
                       &jetstream_error);
    ConsumerInfoPtr created(raw);
    if (status == NATS_OK && created != nullptr && created->Config != nullptr) {
        if (!validate_consumer(options, *created->Config)) return false;
        info("nats_bar_consumer_created",
             {{"stream", options.input_stream}, {"durable", options.durable}});
        return true;
    }

    // A concurrent create is safe only when the winning configuration matches.
    auto raced = lookup_consumer(jetstream, options.input_stream, options.durable);
    if (raced.status == NATS_OK && raced.info != nullptr &&
        raced.info->Config != nullptr) {
        return validate_consumer(options, *raced.info->Config);
    }
    error("nats_bar_consumer_create_failed",
          {{"stream", options.input_stream},
           {"durable", options.durable},
           {"error_code", natsStatus_GetText(status)},
           {"jetstream_code", static_cast<int>(jetstream_error)}});
    return false;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_options(options, argc, argv)) {
        print_usage(std::cerr);
        return 2;
    }
    if (!validate_options(options)) return 2;

    alttech::log::init("alttech-nats-setup", "cli", "info");
    Session session;
    if (!session.connect(options.url)) return 1;

    // The external stream is always looked up and validated, never created or
    // modified, even in --apply mode.
    const auto input = lookup_stream(session.jetstream(), options.input_stream);
    if (!validate_input_stream(options, input)) return 1;
    if (!ensure_output_stream(session.jetstream(), options)) return 1;
    if (!ensure_consumer(session.jetstream(), options)) return 1;

    info("nats_setup_done",
         {{"mode", options.mode == Mode::apply         ? "create_only"
                   : options.mode == Mode::reconcile_limits ? "reconcile_limits"
                   : options.mode == Mode::recreate_consumer ? "recreate_consumer"
                                                             : "read_only"},
          {"input_stream", options.input_stream},
          {"output_stream", options.output_stream},
          {"durable", options.durable}});
    return 0;
}
