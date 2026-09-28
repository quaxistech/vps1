// altspotter nats-init: idempotent market-data JetStream provisioning.
//
//   altspotter nats-init [--url nats://...]   (fallback: ALTSPOTTER_NATS_URL)
//
// Creates (never destroys):
//   * stream ALTSPOTTER_BARS    { bar.> }            file, 30d, 2GiB, discard old, dedup 10m
//   * stream ALTSPOTTER_CONTROL { cmd.>, evt.instrument.>, evt.history.>, evt.integrity.>,
//                          evt.gateway.>, evt.storage.> } file, 30d, 1GiB
//   * stream ALTSPOTTER_MARKET_CONTEXT { md.*.*.{bbo,ticker,mark,index,funding,open_interest},
//                          evt.universe.> }   file, 14d, 2GiB, discard old
//   * durable pull consumers:
//       ALTSPOTTER_BARS/storage-writer-bars
//       ALTSPOTTER_CONTROL/history-repair            (cmd.history.>)
//       ALTSPOTTER_CONTROL/integrity-repair-events   (evt.history.>)
//       ALTSPOTTER_CONTROL/aggregation-repair-events (evt.history.>)
//
// Existing streams are updated only with compatible settings; existing
// consumers are left untouched. Non-zero exit on any critical failure.

#include "ops/ops.hpp"
#include "logging/logger.hpp"

#include <nats.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using altspotter::log::error;
using altspotter::log::info;

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

namespace altspotter::ops {
int cmd_nats_init(int argc, char** argv) {
    std::string url;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--url" && i + 1 < argc) {
            url = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "usage: altspotter nats-init [--url NATS_URL]\n";
            return 0;
        }
    }
    if (url.empty()) {
        if (const char* env = std::getenv("ALTSPOTTER_NATS_URL")) url = env;
    }
    altspotter::log::init("altspotter-nats-init", "cli", "info");
    if (url.empty()) {
        error("nats_init_config_error", {{"message", "no URL: use --url or ALTSPOTTER_NATS_URL"}});
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

    // Retention on these streams is a REPLAY window, not an archive. Consumers
    // process a bar within seconds and PostgreSQL is the durable history; the
    // only thing the stream has to survive is a consumer being down. Two days
    // covers the trading horizon and any restart, where the previous 30 days
    // pinned ~7 GB of JetStream store on disk for messages nothing would ever
    // read again — while the database budget was shedding live order books to
    // stay under its own limit.
    constexpr int64_t kReplayWindow = 2 * kDay;

    bool ok = true;
    ok = ensure_stream(js, "ALTSPOTTER_BARS", {"bar.>"}, kReplayWindow, 2 * kGiB,
                       10LL * 60 * 1000 * 1000 * 1000) && ok;
    ok = ensure_stream(js, "ALTSPOTTER_CONTROL",
                       {"cmd.history.>", "evt.instrument.>", "evt.history.>",
                        "evt.integrity.>", "evt.gateway.>", "evt.storage.>"},
                       30 * kDay, kGiB, 2LL * 60 * 1000 * 1000 * 1000) && ok;
    // Multi-venue derivatives context: BBO/ticker/mark/index/funding/OI plus
    // global-universe events. This is the highest-rate stream on the box, so it
    // gets the same replay window and a tighter cap; publishers throttle
    // BBO/ticker at the source. Raw trades and full book deltas stay on Core
    // NATS only.
    ok = ensure_stream(js, "ALTSPOTTER_MARKET_CONTEXT",
                       {"md.*.*.bbo", "md.*.*.ticker", "md.*.*.mark", "md.*.*.index",
                        "md.*.*.funding", "md.*.*.open_interest", "evt.universe.>"},
                       kReplayWindow, kGiB, 2LL * 60 * 1000 * 1000 * 1000) && ok;
    if (ok) {
        ok = ensure_consumer(js, "ALTSPOTTER_BARS", "storage-writer-bars", nullptr) && ok;
        ok = ensure_consumer(js, "ALTSPOTTER_CONTROL", "history-repair", "cmd.history.>") && ok;
        ok = ensure_consumer(js, "ALTSPOTTER_CONTROL", "integrity-repair-events", "evt.history.>") && ok;
        ok = ensure_consumer(js, "ALTSPOTTER_CONTROL", "aggregation-repair-events", "evt.history.>") && ok;
    }

    jsCtx_Destroy(js);
    natsConnection_Close(conn);
    natsConnection_Destroy(conn);
    natsOptions_Destroy(opts);

    if (!ok) {
        error("nats_init_failed", {{"message", "one or more resources could not be provisioned"}});
        return 1;
    }
    info("nats_init_done", {{"streams", 3}, {"consumers", 4}});
    return 0;
}

}  // namespace altspotter::ops
