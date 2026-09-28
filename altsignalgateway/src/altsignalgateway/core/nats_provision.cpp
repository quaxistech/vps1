// Idempotent JetStream provisioning for the AltSignalGateway-owned topology.

#include "altsignalgateway/core/nats_provision.hpp"

#include "altsignalgateway/core/logger.hpp"
#include "altsignalgateway/core/shutdown.hpp"

#include <nats.h>

#include <cstring>
#include <string>
#include <vector>

using altsignalgateway::log::error;
using altsignalgateway::log::info;

namespace altsignalgateway::messaging {

namespace {

bool subjects_cover(const jsStreamConfig& actual, const char* wanted_root) {
    // The stream must be bound to a subject space that includes the one this
    // consumer filters on. Exact-matching the producer's full subject list
    // would make this service refuse to start every time AltSignalQuality added
    // an unrelated subject to its own stream.
    for (int i = 0; i < actual.SubjectsLen; ++i) {
        if (actual.Subjects[i] == nullptr) continue;
        const std::string s = actual.Subjects[i];
        if (s == wanted_root) return true;
        // "signalquality.>" covers "signalquality.final.>".
        if (s.size() >= 2 && s.compare(s.size() - 2, 2, ".>") == 0) {
            const std::string prefix = s.substr(0, s.size() - 1);  // keep the dot
            if (std::string(wanted_root).compare(0, prefix.size(), prefix) == 0) return true;
        }
    }
    return false;
}

bool require_stream(jsCtx* js, const char* name, const char* subject_root) {
    constexpr int kMaxAttempts = 20;
    jsErrCode jerr = static_cast<jsErrCode>(0);
    jsStreamInfo* si = nullptr;
    natsStatus s = NATS_ERR;
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        jerr = static_cast<jsErrCode>(0);
        si = nullptr;
        s = js_GetStreamInfo(&si, js, name, nullptr, &jerr);
        if (s == NATS_OK && si != nullptr) break;
        if (si != nullptr) {
            jsStreamInfo_Destroy(si);
            si = nullptr;
        }
        const bool absent = s == NATS_NOT_FOUND || jerr == JSStreamNotFoundErr;
        if (!absent || attempt == kMaxAttempts || ShutdownToken::instance().stop_requested()) {
            error("required_stream_missing", {{"stream", name},
                                              {"error_code", natsStatus_GetText(s)},
                                              {"jetstream_code", static_cast<int>(jerr)},
                                              {"attempts", attempt}});
            return false;
        }
        if (attempt == 1) info("required_stream_wait", {{"stream", name}});
        nats_Sleep(1000);
    }

    // What a CONSUMER may require of a stream it does not own is the shape that
    // affects CORRECTNESS: that it carries the subjects being read, that it is
    // durable, that it discards the oldest rather than refusing new publishes,
    // and that it acknowledges. Capacity — MaxAge, MaxBytes — is the OWNER's
    // operational decision; pinning it here would make this service refuse to
    // start the moment AltSignalQuality retuned its own retention, which is a
    // consumer vetoing a producer's capacity planning.
    const jsStreamConfig* actual = si->Config;
    const bool shape_ok = actual != nullptr && subjects_cover(*actual, subject_root) &&
                          actual->Storage == js_FileStorage &&
                          actual->Retention == js_LimitsPolicy &&
                          actual->Discard == js_DiscardOld && !actual->NoAck;
    // A replay window shorter than this cannot survive a restart of this
    // service, which is a genuine contract violation rather than a preference.
    constexpr int64_t kMinReplayWindowNs = 6LL * 60 * 60 * 1000 * 1000 * 1000;
    const bool window_ok =
        actual != nullptr && (actual->MaxAge == 0 || actual->MaxAge >= kMinReplayWindowNs);
    if (!shape_ok || !window_ok) {
        error("required_stream_config_drift",
              {{"stream", name},
               {"message",
                "externally owned stream does not match the AltSignalGateway consumer contract"},
               {"subject_root", subject_root},
               {"subjects_ok", actual != nullptr && subjects_cover(*actual, subject_root)},
               {"window_ok", window_ok},
               {"actual_max_age_h",
                actual != nullptr ? actual->MaxAge / 3'600'000'000'000LL : -1}});
        jsStreamInfo_Destroy(si);
        return false;
    }
    jsStreamInfo_Destroy(si);
    info("required_stream_validated", {{"stream", name}, {"subject_root", subject_root}});
    return true;
}

bool ensure_consumer(jsCtx* js, const char* stream, const char* durable, const char* filter,
                     jsDeliverPolicy deliver_policy) {
    jsConsumerConfig desired;
    jsConsumerConfig_Init(&desired);
    desired.Durable = durable;
    desired.FilterSubject = filter;
    desired.AckPolicy = js_AckExplicit;
    desired.DeliverPolicy = deliver_policy;
    desired.MaxDeliver = -1;
    // 60s is generous for "parse, enrich with two local reads, write one row".
    // Making it shorter would let a slow database turn into a redelivery storm
    // on top of a slow database.
    desired.AckWait = 60LL * 1000 * 1000 * 1000;
    desired.MaxAckPending = 1024;

    jsErrCode jerr = static_cast<jsErrCode>(0);
    jsConsumerInfo* ci = nullptr;
    natsStatus s = js_GetConsumerInfo(&ci, js, stream, durable, nullptr, &jerr);
    if (s == NATS_OK && ci != nullptr) {
        const auto same_text = [](const char* lhs, const char* rhs) {
            if (lhs == nullptr || *lhs == '\0') return rhs == nullptr || *rhs == '\0';
            return rhs != nullptr && std::strcmp(lhs, rhs) == 0;
        };
        const jsConsumerConfig* actual = ci->Config;
        // DeliverPolicy is deliberately NOT compared. It only applies when the
        // durable is created, and JetStream reports an existing durable's
        // policy as whatever it was created with — comparing it would make a
        // service configured for `new` refuse to bind to a durable someone
        // provisioned with `all`, for a setting that has no effect any more.
        const bool matches = actual != nullptr && actual->AckPolicy == desired.AckPolicy &&
                             actual->AckWait == desired.AckWait &&
                             actual->MaxDeliver == desired.MaxDeliver &&
                             actual->MaxAckPending == desired.MaxAckPending &&
                             actual->DeliverSubject == nullptr &&
                             same_text(actual->FilterSubject, desired.FilterSubject);
        if (!matches) {
            error("consumer_config_drift",
                  {{"stream", stream},
                   {"durable", durable},
                   {"message", "existing durable does not match the declared contract"}});
            jsConsumerInfo_Destroy(ci);
            return false;
        }
        jsConsumerInfo_Destroy(ci);
        info("consumer_exists", {{"stream", stream}, {"durable", durable}});
        return true;
    }

    ci = nullptr;
    s = js_AddConsumer(&ci, js, stream, &desired, nullptr, &jerr);
    if (s != NATS_OK) {
        error("consumer_create_failed", {{"stream", stream},
                                         {"durable", durable},
                                         {"error_code", natsStatus_GetText(s)},
                                         {"jetstream_code", static_cast<int>(jerr)}});
        return false;
    }
    jsConsumerInfo_Destroy(ci);
    info("consumer_created",
         {{"stream", stream},
          {"durable", durable},
          {"deliver_policy", deliver_policy == js_DeliverAll ? "all" : "new"}});
    return true;
}

} // namespace

bool nats_provision(const std::string& url, const config::WiringConfig& wiring, bool deliver_all) {
    natsConnection* conn = nullptr;
    natsOptions* opts = nullptr;
    natsStatus s = natsOptions_Create(&opts);
    if (s == NATS_OK) s = natsOptions_SetURL(opts, url.c_str());
    if (s == NATS_OK) s = natsOptions_SetTimeout(opts, 5000);
    if (s != NATS_OK) {
        error("nats_options_failed", {{"error_code", natsStatus_GetText(s)}});
        natsOptions_Destroy(opts);
        return false;
    }
    // systemd's After=nats-server.service waits for the process, not for
    // JetStream to accept connections, so a cold boot races this step.
    constexpr int kMaxConnectAttempts = 30;
    for (int attempt = 1;; ++attempt) {
        s = natsConnection_Connect(&conn, opts);
        if (s == NATS_OK) break;
        if (attempt >= kMaxConnectAttempts || ShutdownToken::instance().stop_requested()) {
            error("nats_connect_failed",
                  {{"error_code", natsStatus_GetText(s)}, {"attempts", attempt}});
            natsOptions_Destroy(opts);
            return false;
        }
        nats_Sleep(attempt < 8 ? attempt * 250 : 2000);
    }

    jsCtx* js = nullptr;
    s = natsConnection_JetStream(&js, conn, nullptr);
    if (s != NATS_OK) {
        error("jetstream_context_failed", {{"error_code", natsStatus_GetText(s)}});
        natsConnection_Destroy(conn);
        natsOptions_Destroy(opts);
        return false;
    }

    bool ok = require_stream(js, wiring.final_stream.c_str(),
                             wiring.final_subject_filter.c_str());
    if (ok) {
        ok = ensure_consumer(js, wiring.final_stream.c_str(), wiring.final_durable.c_str(),
                             wiring.final_subject_filter.c_str(),
                             deliver_all ? js_DeliverAll : js_DeliverNew);
    }

    jsCtx_Destroy(js);
    natsConnection_Close(conn);
    natsConnection_Destroy(conn);
    natsOptions_Destroy(opts);

    if (!ok) {
        error("nats_init_failed",
              {{"message", "the input stream or the durable consumer could not be validated"}});
        return false;
    }
    info("nats_init_done", {{"owned_streams", 0},
                            {"required_streams", 1},
                            {"consumers", 1},
                            {"input_stream", wiring.final_stream},
                            {"durable", wiring.final_durable}});
    return true;
}

} // namespace altsignalgateway::messaging
