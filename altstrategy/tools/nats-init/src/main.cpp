// altstrategy nats-init: idempotent JetStream provisioning for the self-contained
// decision engine. AltStrategy owns exactly ONE stream — its durable DECISION
// OUTPUT for VPS2 live incremental sync (§31). It never creates or requires the
// upstream streams (AltSpotter / AltTech / AltML own those); the engine consumes
// inputs over core NATS, so VPS1 stays self-contained.
//
//   altstrategy-nats-init [--url nats://...]   (fallback: ALTSTRATEGY_NATS_URL)
//
//   * ALTSTRATEGY_DECISIONS { strategy.>, tradeplan.>, tradeproposal.>, signal.>,
//                             archive.snapshot.> }   (env: ALTSTRATEGY_OUTPUT_STREAM)
//     Stream byte cap defaults to a modest 256 MiB (plenty for lightweight JSON/
//     protobuf decision envelopes); raise it for production via the
//     ALTSTRATEGY_OUTPUT_STREAM_MAX_BYTES env var (bytes) once the disk budget is
//     sized, with no code change.
//
// Existing streams are updated only with compatible settings. Non-zero exit on
// any critical failure.

#include "altstrategy/logging/logger.hpp"

#include <nats.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using altstrategy::log::error;
using altstrategy::log::info;

namespace {

constexpr int64_t kMiB = 1024LL * 1024;
constexpr int64_t kDefaultOutputStreamMaxBytes = 256LL * kMiB;
// The stream drops a repeated Nats-Msg-Id only inside this window, and the
// service's outbox retries an unacknowledged message for up to
// publication.max_age_hours (24): a publish whose ack was lost and that is
// retried later than the window would otherwise be stored twice.
constexpr int64_t kOutputDuplicateWindowNs = 24LL * 3600 * 1000 * 1000 * 1000;

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

[[maybe_unused]] bool subjects_match(const jsStreamConfig& actual,
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

[[maybe_unused]] bool require_stream(jsCtx* js, const char* name,
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
        if (!absent || attempt == kMaxAttempts) {
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

    const jsStreamConfig* actual = si->Config;
    const bool matches = actual != nullptr &&
                         subjects_match(*actual, subjects) &&
                         actual->Storage == js_FileStorage &&
                         actual->Retention == js_LimitsPolicy &&
                         actual->Discard == js_DiscardOld &&
                         actual->MaxAge == max_age_ns &&
                         actual->MaxBytes == max_bytes &&
                         actual->Duplicates == dup_window_ns &&
                         actual->Replicas == 1 &&
                         !actual->NoAck;
    if (!matches) {
        error("required_stream_config_drift",
              {{"stream", name},
               {"message", "Externally owned stream does not match the AltStrategy consumer contract"}});
        jsStreamInfo_Destroy(si);
        return false;
    }

    jsStreamInfo_Destroy(si);
    info("required_stream_validated", {{"stream", name}});
    return true;
}

[[maybe_unused]] bool ensure_consumer(jsCtx* js, const char* stream, const char* durable, const char* filter,
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

int main(int argc, char** argv) {
    std::string url;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--url" && i + 1 < argc) {
            url = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "usage: altstrategy-nats-init [--url NATS_URL]\n";
            return 0;
        }
    }
    if (url.empty()) {
        if (const char* env = std::getenv("ALTSTRATEGY_NATS_URL")) url = env;
    }
    altstrategy::log::init("altstrategy-nats-init", "cli", "info");
    if (url.empty()) {
        error("nats_init_config_error", {{"message", "no URL: use --url or ALTSTRATEGY_NATS_URL"}});
        return 2;
    }

    natsConnection* conn = nullptr;
    natsOptions* opts = nullptr;
    natsStatus s = natsOptions_Create(&opts);
    if (s == NATS_OK) s = natsOptions_SetURL(opts, url.c_str());
    if (s == NATS_OK) s = natsOptions_SetTimeout(opts, 5000);
    if (s != NATS_OK) {
        error("nats_options_failed", {{"error_code", natsStatus_GetText(s)}});
        natsOptions_Destroy(opts);
        return 1;
    }
    // systemd's After=nats-server.service only waits for the server process to
    // start, not for JetStream to accept connections, so a cold boot races the
    // provisioning oneshot ("No server available"). Retry the initial connect a
    // bounded number of times with capped backoff before failing.
    constexpr int kMaxConnectAttempts = 30;
    for (int attempt = 1;; ++attempt) {
        s = natsConnection_Connect(&conn, opts);
        if (s == NATS_OK) break;
        if (attempt >= kMaxConnectAttempts) {
            error("nats_connect_failed",
                  {{"error_code", natsStatus_GetText(s)}, {"attempts", attempt}});
            natsOptions_Destroy(opts);
            return 1;
        }
        nats_Sleep(attempt < 8 ? attempt * 250 : 2000); // 250ms..2s capped backoff
    }

    jsCtx* js = nullptr;
    s = natsConnection_JetStream(&js, conn, nullptr);
    if (s != NATS_OK) {
        error("jetstream_context_failed", {{"error_code", natsStatus_GetText(s)}});
        natsConnection_Destroy(conn);
        natsOptions_Destroy(opts);
        return 1;
    }

    constexpr int64_t kDay = 24LL * 3600 * 1000 * 1000 * 1000;
    // AltStrategy owns exactly ONE JetStream stream: its durable decision output
    // for VPS2 live incremental sync (§31). It captures the AltStrategy-published
    // subjects only; the input streams (alttech/altml/altspotter) belong to those
    // services and are never created or required here (VPS1 self-containment). The
    // orchestrator consumes inputs over core NATS, so no cross-service consumer is
    // provisioned. Stream name is env-overridable.
    const char* out_env = std::getenv("ALTSTRATEGY_OUTPUT_STREAM");
    const std::string output_stream = out_env ? out_env : "ALTSTRATEGY_DECISIONS";
    int64_t output_stream_max_bytes = kDefaultOutputStreamMaxBytes;
    if (const char* bytes_env = std::getenv("ALTSTRATEGY_OUTPUT_STREAM_MAX_BYTES")) {
        try { output_stream_max_bytes = std::stoll(bytes_env); } catch (...) {}
    }
    bool ok = true;
    ok = ensure_stream(js, output_stream.c_str(),
                       {"strategy.>", "tradeplan.>", "tradeproposal.>", "signal.>",
                        "archive.snapshot.>"},
                       30 * kDay, output_stream_max_bytes, kOutputDuplicateWindowNs) && ok;

    jsCtx_Destroy(js);
    natsConnection_Close(conn);
    natsConnection_Destroy(conn);
    natsOptions_Destroy(opts);

    if (!ok) {
        error("nats_init_failed",
              {{"message", "one or more resources could not be provisioned or validated"}});
        return 1;
    }
    info("nats_init_done",
         {{"owned_streams", 1}, {"required_streams", 0}, {"consumers", 0}});
    return 0;
}
