// Idempotent JetStream provisioning for the AltML-owned topology.
//
// Owns the output stream (predictions), the control stream (model lifecycle)
// and the inference durable. Requires, but never creates or updates, the
// input stream (ALTTECH_ANALYTICS by default — owned by the AltTech feature
// producer). Existing streams are updated only with compatible settings;
// existing consumers are left untouched.

#include "altml/core/nats_provision.hpp"

#include "altml/core/logger.hpp"
#include "altml/core/shutdown.hpp"

#include <nats.h>

#include <cstring>
#include <string>
#include <vector>

using altml::log::error;
using altml::log::info;

namespace altml::messaging {

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

// Validate a stream this service does NOT own. Retention size and age are the
// owner's decision, so they are deliberately not asserted here -- only what
// AltML actually depends on: the subjects it reads, a replay window long enough
// to survive a restart, and a dedup window at least as long as the producer's.
//
// It used to take max_age_ns and max_bytes and ignore both, so the call site
// read "30 days / 3 GiB" while the check accepted anything over six hours. The
// arguments are gone rather than silenced: a parameter that cannot affect the
// outcome is a claim the code does not keep.
bool require_stream(jsCtx* js, const char* name,
                    const std::vector<const char*>& subjects,
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
               {"message", "externally owned stream does not match the AltML consumer contract"},
               {"subjects_ok", actual != nullptr && subjects_match(*actual, subjects)},
               {"window_ok", window_ok},
               {"actual_max_age_h", actual != nullptr ? actual->MaxAge / 3'600'000'000'000LL : -1}});
        jsStreamInfo_Destroy(si);
        return false;
    }
    jsStreamInfo_Destroy(si);
    info("required_stream_validated", {{"stream", name}});
    return true;
}

bool ensure_consumer(jsCtx* js, const char* stream, const char* durable,
                     const std::vector<std::string>& filters,
                     bool widen_input_windows_stopped = false,
                     jsDeliverPolicy deliver_policy = js_DeliverAll) {
    jsConsumerConfig desired;
    jsConsumerConfig_Init(&desired);
    desired.Durable = durable;
    // One filter goes in FilterSubject, several in FilterSubjects (server 2.10+).
    // Setting both is rejected by the server, so exactly one form is used.
    std::vector<const char*> filter_ptrs;
    filter_ptrs.reserve(filters.size());
    for (const auto& f : filters) filter_ptrs.push_back(f.c_str());
    if (filters.size() == 1) {
        desired.FilterSubject = filter_ptrs[0]; // nullptr = all stream subjects
    } else if (filters.size() > 1) {
        desired.FilterSubjects = filter_ptrs.data();
        desired.FilterSubjectsLen = static_cast<int>(filter_ptrs.size());
    }
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
        // Drift is checked against whichever form is in use. A durable whose
        // subjects differ from the declared ones is refused, not silently
        // reused: it would deliver a different slice of the stream than the
        // service believes it is reading.
        const auto same_filters = [&same_text](const jsConsumerConfig* lhs,
                                               const jsConsumerConfig& rhs) {
            if (rhs.FilterSubjectsLen != lhs->FilterSubjectsLen) return false;
            if (rhs.FilterSubjectsLen == 0) {
                return same_text(lhs->FilterSubject, rhs.FilterSubject);
            }
            for (int i = 0; i < rhs.FilterSubjectsLen; ++i) {
                if (!same_text(lhs->FilterSubjects[i], rhs.FilterSubjects[i])) return false;
            }
            return true;
        };
        const jsConsumerConfig* actual = ci->Config;
        const bool profile_matches = actual != nullptr &&
                             actual->AckPolicy == desired.AckPolicy &&
                             actual->DeliverPolicy == desired.DeliverPolicy &&
                             actual->AckWait == desired.AckWait &&
                             actual->MaxDeliver == desired.MaxDeliver &&
                             actual->MaxAckPending == desired.MaxAckPending &&
                             actual->DeliverSubject == nullptr;
        const bool matches = profile_matches && same_filters(actual, desired);
        if (!matches && widen_input_windows_stopped && profile_matches &&
            filters == std::vector<std::string>{"alttech.feature.>", "alttech.microfeature.venue.>"} &&
            actual->FilterSubjectsLen == 2 && actual->FilterSubjects != nullptr &&
            same_text(actual->FilterSubjects[0], "alttech.feature.>") &&
            same_text(actual->FilterSubjects[1], "alttech.microfeature.venue.*.*.1m.v1")) {
            // Maintenance precondition: stop the daemon and let its current
            // receipts settle. No consumer deletion, start-sequence override,
            // acknowledgement, message deletion or synthetic replay occurs.
            if (ci->NumAckPending != 0 || ci->NumWaiting != 0 ||
                ci->Delivered.Stream != ci->AckFloor.Stream) {
                error("input_window_migration_refused", {{"reason", "consumer_not_quiescent"}});
                jsConsumerInfo_Destroy(ci);
                return false;
            }
            const auto ack_before = ci->AckFloor.Stream;
            const auto delivered_before = ci->Delivered.Stream;
            jsConsumerConfig widened = *actual;
            widened.FilterSubject = nullptr;
            widened.FilterSubjects = desired.FilterSubjects;
            widened.FilterSubjectsLen = desired.FilterSubjectsLen;
            jsConsumerInfo* updated = nullptr;
            s = js_UpdateConsumer(&updated, js, stream, &widened, nullptr, &jerr);
            const bool preserved = s == NATS_OK && updated != nullptr && updated->Config != nullptr &&
                same_filters(updated->Config, desired) && updated->AckFloor.Stream == ack_before &&
                updated->Delivered.Stream == delivered_before && updated->NumAckPending == 0;
            jsConsumerInfo_Destroy(ci);
            if (updated != nullptr) jsConsumerInfo_Destroy(updated);
            if (!preserved) {
                error("input_window_migration_unverified", {{"durable", durable},
                    {"error_code", natsStatus_GetText(s)}});
                return false; // inspect actual state; never recreate a durable
            }
            info("input_window_filters_widened", {{"durable", durable},
                {"cursor_preserved", true}, {"historical_fast_replay", false}});
            return true;
        }
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

bool nats_provision(const std::string& url, const config::ProjectConfig& project,
                    bool widen_input_windows_stopped) {
    if (widen_input_windows_stopped && project.input_subject_filters !=
        std::vector<std::string>{"alttech.feature.>", "alttech.microfeature.venue.>"}) {
        error("input_window_migration_refused", {{"reason", "unsupported_target_filters"}});
        return false;
    }
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
    // start, not for JetStream to accept connections, so a cold boot races the
    // provisioning step ("No server available"). Retry the initial connect a
    // bounded number of times with capped backoff before failing.
    constexpr int kMaxConnectAttempts = 30;
    for (int attempt = 1;; ++attempt) {
        s = natsConnection_Connect(&conn, opts);
        if (s == NATS_OK) break;
        // Honor SIGTERM during startup provisioning: `altml run` installs the
        // shutdown handler before calling this, and a stop request must not
        // wait for the remaining retry budget.
        if (attempt >= kMaxConnectAttempts || ShutdownToken::instance().stop_requested()) {
            error("nats_connect_failed",
                  {{"error_code", natsStatus_GetText(s)}, {"attempts", attempt}});
            natsOptions_Destroy(opts);
            return false;
        }
        nats_Sleep(attempt < 8 ? attempt * 250 : 2000); // 250ms..2s capped backoff
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
    std::vector<const char*> control_subjects;
    control_subjects.reserve(project.control_subjects.size());
    for (const auto& subject : project.control_subjects) {
        control_subjects.push_back(subject.c_str());
    }

    bool ok = true;
    // The input stream is externally owned by the feature producer (AltTech):
    // validated, never created, so its cursors and subjects are preserved. Its
    // retention (AltTech's canonical topology v1 is 30 days / 3 GiB) belongs to
    // its owner; what AltML requires is the subject set and a 2-minute dedup
    // window matching the producer's message ids.
    ok = require_stream(js, project.input_stream.c_str(), {"alttech.>"},
                        2LL * 60 * 1000 * 1000 * 1000) && ok;
    // AltML owns its output and control streams. Consuming applications (HFT
    // once it adapts, or any other project) create their own durables on the
    // output stream; they never provision it.
    ok = ensure_stream(js, project.output_stream.c_str(), {"prediction.>"},
                       30 * kDay, kGiB, 2LL * 60 * 1000 * 1000 * 1000) && ok;
    ok = ensure_stream(js, project.control_stream.c_str(), control_subjects,
                       30 * kDay, 512 * 1024 * 1024, 2LL * 60 * 1000 * 1000 * 1000) && ok;

    if (ok) {
        ok = ensure_consumer(js, project.input_stream.c_str(), project.durable.c_str(),
                             project.input_subject_filters, widen_input_windows_stopped) && ok;
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
         {{"owned_streams", 2}, {"required_streams", 1}, {"consumers", 1}});
    return true;
}

} // namespace altml::messaging
