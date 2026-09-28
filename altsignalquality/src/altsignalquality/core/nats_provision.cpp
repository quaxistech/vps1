// Idempotent JetStream provisioning for the AltSignalQuality-owned topology.
//
// Owns the output stream (final signals, evaluations, model lifecycle) and the
// durable consumer on AltStrategy's decision stream. Requires, but never
// creates or updates, that input stream: its subjects, retention and cursors
// belong to AltStrategy, and a consumer must not silently retune a producer.

#include "altsignalquality/core/nats_provision.hpp"

#include "altsignalquality/core/logger.hpp"
#include "altsignalquality/core/shutdown.hpp"

#include <nats.h>

#include <cstring>
#include <string>
#include <vector>

using altsignalquality::log::error;
using altsignalquality::log::info;

namespace altsignalquality::messaging {

namespace {

constexpr int64_t kGiB = 1024LL * 1024 * 1024;

bool ensure_stream(jsCtx* js, const char* name, const std::vector<const char*>& subjects,
                   int64_t max_age_ns, int64_t max_bytes, int64_t dup_window_ns) {
    jsStreamConfig cfg;
    jsStreamConfig_Init(&cfg);
    cfg.Name = name;
    cfg.Subjects = const_cast<const char**>(subjects.data());
    cfg.SubjectsLen = static_cast<int>(subjects.size());
    cfg.Storage = js_FileStorage;
    cfg.MaxAge = max_age_ns;
    cfg.MaxBytes = max_bytes;
    cfg.Discard = js_DiscardOld;
    cfg.Duplicates = dup_window_ns;
    cfg.Replicas = 1;
    cfg.Retention = js_LimitsPolicy;

    jsErrCode jerr = static_cast<jsErrCode>(0);
    jsStreamInfo* si = nullptr;
    natsStatus s = js_GetStreamInfo(&si, js, name, nullptr, &jerr);
    if (s == NATS_OK && si != nullptr) {
        jsStreamInfo_Destroy(si);
        si = nullptr;
        s = js_UpdateStream(&si, js, &cfg, nullptr, &jerr);
        if (s != NATS_OK) {
            error("stream_update_failed", {{"stream", name},
                                           {"error_code", natsStatus_GetText(s)},
                                           {"jetstream_code", static_cast<int>(jerr)}});
            return false;
        }
        info("stream_updated", {{"stream", name}});
    } else if (s == NATS_NOT_FOUND || jerr == JSStreamNotFoundErr) {
        s = js_AddStream(&si, js, &cfg, nullptr, &jerr);
        if (s != NATS_OK) {
            error("stream_create_failed", {{"stream", name},
                                           {"error_code", natsStatus_GetText(s)},
                                           {"jetstream_code", static_cast<int>(jerr)}});
            return false;
        }
        info("stream_created", {{"stream", name}});
    } else {
        error("stream_info_failed", {{"stream", name}, {"error_code", natsStatus_GetText(s)}});
        return false;
    }
    if (si != nullptr) jsStreamInfo_Destroy(si);
    return true;
}

bool subjects_match(const jsStreamConfig& actual,
                    const std::vector<const char*>& expected) {
    if (actual.SubjectsLen != static_cast<int>(expected.size()) ||
        (actual.SubjectsLen > 0 && actual.Subjects == nullptr)) {
        return false;
    }
    for (const char* wanted : expected) {
        bool found = false;
        for (int i = 0; i < actual.SubjectsLen; ++i) {
            if (actual.Subjects[i] != nullptr &&
                std::strcmp(actual.Subjects[i], wanted) == 0) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

bool require_stream(jsCtx* js, const char* name,
                    const std::vector<const char*>& subjects,
                    int64_t max_age_ns, int64_t max_bytes,
                    int64_t dup_window_ns) {
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
        if (!absent || attempt == kMaxAttempts ||
            ShutdownToken::instance().stop_requested()) {
            error("required_stream_missing",
                  {{"stream", name},
                   {"error_code", natsStatus_GetText(s)},
                   {"jetstream_code", static_cast<int>(jerr)},
                   {"attempts", attempt}});
            return false;
        }
        if (attempt == 1) {
            info("required_stream_wait", {{"stream", name}});
        }
        nats_Sleep(1000);
    }

    // What a CONSUMER may require of a stream it does not own is the shape that
    // affects correctness: the subjects it reads, durable file storage, a limits
    // retention policy that discards the oldest rather than rejecting new
    // publishes, acknowledgements, and a de-duplication window at least as wide
    // as the one it relies on.
    //
    // Capacity — MaxAge and MaxBytes — is the OWNER's operational decision. It
    // is sized from that service's disk budget and its own replay needs, and it
    // changes without the wire contract changing. Pinning exact values here made
    // AltML refuse to start the moment AltTech retuned its retention, which is a
    // consumer vetoing a producer's capacity planning. What AltML actually needs
    // is that the window is not so short that a restart loses unread messages.
    const jsStreamConfig* actual = si->Config;
    const bool shape_ok = actual != nullptr &&
                          subjects_match(*actual, subjects) &&
                          actual->Storage == js_FileStorage &&
                          actual->Retention == js_LimitsPolicy &&
                          actual->Discard == js_DiscardOld &&
                          actual->Duplicates >= dup_window_ns &&
                          actual->Replicas == 1 &&
                          !actual->NoAck;
    // A replay window shorter than this cannot survive a restart of this
    // service, so it is a genuine contract violation rather than a preference.
    constexpr int64_t kMinReplayWindowNs = 6LL * 60 * 60 * 1000 * 1000 * 1000;
    const bool window_ok =
        actual != nullptr && (actual->MaxAge == 0 || actual->MaxAge >= kMinReplayWindowNs);
    if (!shape_ok || !window_ok) {
        error("required_stream_config_drift",
              {{"stream", name},
               {"message", "externally owned stream does not match the AltSignalQuality consumer contract"},
               {"subjects_ok", actual != nullptr && subjects_match(*actual, subjects)},
               {"window_ok", window_ok},
               {"actual_max_age_h", actual != nullptr ? actual->MaxAge / 3'600'000'000'000LL : -1}});
        jsStreamInfo_Destroy(si);
        return false;
    }
    (void)max_age_ns;
    (void)max_bytes;

    jsStreamInfo_Destroy(si);
    info("required_stream_validated", {{"stream", name}});
    return true;
}

bool ensure_consumer(jsCtx* js, const char* stream, const char* durable, const char* filter,
                     jsDeliverPolicy deliver_policy = js_DeliverAll) {
    jsConsumerConfig desired;
    jsConsumerConfig_Init(&desired);
    desired.Durable = durable;
    desired.FilterSubject = filter; // nullptr = all stream subjects
    desired.AckPolicy = js_AckExplicit;
    desired.DeliverPolicy = deliver_policy;
    desired.MaxDeliver = -1;
    desired.AckWait = 60LL * 1000 * 1000 * 1000; // 60s in ns
    desired.MaxAckPending = 4096;

    jsErrCode jerr = static_cast<jsErrCode>(0);
    jsConsumerInfo* ci = nullptr;
    natsStatus s = js_GetConsumerInfo(&ci, js, stream, durable, nullptr, &jerr);
    if (s == NATS_OK && ci != nullptr) {
        const auto same_text = [](const char* lhs, const char* rhs) {
            if (lhs == nullptr || *lhs == '\0') return rhs == nullptr || *rhs == '\0';
            return rhs != nullptr && std::strcmp(lhs, rhs) == 0;
        };
        const jsConsumerConfig* actual = ci->Config;
        const bool matches = actual != nullptr &&
                             actual->AckPolicy == desired.AckPolicy &&
                             actual->DeliverPolicy == desired.DeliverPolicy &&
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
    info("consumer_created", {{"stream", stream}, {"durable", durable}});
    return true;
}


} // namespace

bool nats_provision(const std::string& url, const config::WiringConfig& wiring) {
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
    // systemd's After=nats-server.service only waits for the server process to
    // start, not for JetStream to accept connections, so a cold boot races this
    // step ("No server available"). Retry with capped backoff, honouring a stop
    // request so SIGTERM during startup does not wait out the whole budget.
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

    constexpr int64_t kDay = 24LL * 3600 * 1000 * 1000 * 1000;
    // What this service REQUIRES of the input stream: two minutes is the
    // stack's convention and is all a consumer needs there.
    constexpr int64_t kInputDedupWindowNs = 2LL * 60 * 1000 * 1000 * 1000;
    // What this service's OWN stream needs is different and larger. A crash
    // between publishing a final signal and recording that it was published
    // leaves the row unpublished; startup then re-publishes it under the same
    // deterministic id. With a two-minute window, any restart slower than two
    // minutes — which is most of them — would put a genuine duplicate on the
    // bus and leave de-duplication entirely to the consumer. Two hours covers a
    // restart, a short outage and a rollback, and costs only the message ids
    // JetStream tracks for that window (hundreds a day here).
    constexpr int64_t kOutputDedupWindowNs = 2LL * 3600 * 1000 * 1000 * 1000;

    bool ok = true;
    // AltStrategy owns its decision stream: validate the shape this service
    // depends on (subjects, durable file storage, limits/discard-old, ack, a
    // dedup window at least as wide as ours, a replay window long enough to
    // survive a restart) and never touch its capacity settings.
    ok = require_stream(js, wiring.candidate_stream.c_str(),
                        {"strategy.>", "tradeplan.>", "tradeproposal.>", "signal.>",
                         "archive.snapshot.>"},
                        30 * kDay, kGiB, kInputDedupWindowNs) && ok;

    // AltSignalQuality owns its output stream. VPS2 creates its own durable on
    // it; it is never provisioned from the consumer side.
    std::vector<std::string> roots = wiring.output_subject_roots();
    std::vector<const char*> subjects;
    subjects.reserve(roots.size());
    for (const auto& root : roots) subjects.push_back(root.c_str());
    ok = ensure_stream(js, wiring.output_stream.c_str(), subjects, 30 * kDay,
                       512 * 1024 * 1024, kOutputDedupWindowNs) && ok;

    // The durable this service reads candidates through. Created only after the
    // input stream validated, so a drifted contract fails before a consumer is
    // bound to it.
    //
    // DeliverNew, not DeliverAll. AltStrategy's stream retains weeks of
    // proposals, and none of them can be acted on: a signal older than the
    // freshness window is rejected as SIGNAL_STALE by construction. Replaying
    // them would spend hours of a loaded box producing rejections, while the
    // history this service actually needs comes from PostgreSQL — where the
    // backfill path reconstructs it with genuinely as-of priors, which a replay
    // cannot do. The policy applies only when the durable is CREATED; a restart
    // resumes at the stored cursor, so nothing in flight is lost.
    if (ok) {
        ok = ensure_consumer(js, wiring.candidate_stream.c_str(),
                             wiring.candidate_durable.c_str(),
                             wiring.candidate_subject_filter.c_str(), js_DeliverNew) && ok;
    }

    jsCtx_Destroy(js);
    natsConnection_Close(conn);
    natsConnection_Destroy(conn);
    natsOptions_Destroy(opts);

    if (!ok) {
        error("nats_init_failed",
              {{"message", "one or more resources could not be provisioned or validated"}});
        return false;
    }
    info("nats_init_done",
         {{"owned_streams", 1}, {"required_streams", 1}, {"consumers", 1},
          {"output_stream", wiring.output_stream}, {"input_stream", wiring.candidate_stream}});
    return true;
}

} // namespace altsignalquality::messaging
