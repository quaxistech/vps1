#include "altmacro/core/nats_provision.hpp"

#include "altmacro/core/logger.hpp"

#include <nats/nats.h>

#include <cstring>
#include <string>
#include <vector>

namespace altmacro::messaging {
namespace {

constexpr int64_t kNsPerHour = 3'600'000'000'000LL;

} // namespace

bool nats_provision(const std::string& url, const NatsConfig& cfg) {
    natsOptions* opts = nullptr;
    if (natsOptions_Create(&opts) != NATS_OK) {
        log::error("nats_options_failed", {});
        return false;
    }
    natsOptions_SetURL(opts, url.c_str());
    natsOptions_SetName(opts, "altmacro-provision");
    if (!cfg.credentials_file.empty()) {
        natsOptions_SetUserCredentialsFromFiles(opts, cfg.credentials_file.c_str(), nullptr);
    }

    natsConnection* conn = nullptr;
    natsStatus s = natsConnection_Connect(&conn, opts);
    natsOptions_Destroy(opts);
    if (s != NATS_OK) {
        log::error("nats_connect_failed",
                   {{"url", url}, {"error", natsStatus_GetText(s)}});
        return false;
    }

    jsCtx* js = nullptr;
    jsOptions jsOpts;
    jsOptions_Init(&jsOpts);
    s = natsConnection_JetStream(&js, conn, &jsOpts);
    if (s != NATS_OK) {
        log::error("jetstream_context_failed", {{"error", natsStatus_GetText(s)}});
        natsConnection_Destroy(conn);
        return false;
    }

    const std::string bars = cfg.subject_prefix + ".bar.>";
    const std::string refs = cfg.subject_prefix + ".reference.>";
    std::vector<const char*> subjects{bars.c_str(), refs.c_str()};

    jsStreamConfig sc;
    jsStreamConfig_Init(&sc);
    sc.Name = cfg.stream.c_str();
    sc.Subjects = subjects.data();
    sc.SubjectsLen = static_cast<int>(subjects.size());
    sc.Storage = js_FileStorage;
    sc.Retention = js_LimitsPolicy;
    sc.Discard = js_DiscardOld;
    sc.Replicas = 1;
    // Four weeks. Long enough that a consumer can be down for a fortnight and
    // still catch up from the stream; short enough that this never becomes a
    // second, divergent copy of the history PostgreSQL already holds.
    sc.MaxAge = 28 * 24 * kNsPerHour;
    // Sized from what this service actually emits, and from what the host has
    // left to give.
    //
    // The bootstrap publishes nothing; only the live poller does. That is four
    // instruments on both sides of the book at one bar a minute -- about 11,500
    // messages and some 7 MB a day -- plus a dozen daily reference points. Four
    // weeks of that is roughly 200 MB, so 384 MiB carries the retention with
    // headroom to spare.
    //
    // The ceiling is not a formality. JetStream reserves MaxBytes against a
    // server-wide file budget rather than charging for what is used: on VPS1
    // the eight existing streams already reserve 7.2 of 8 GiB while occupying
    // 1.3, so an over-generous request here is refused outright (JetStream
    // error 10047) and a merely large one would crowd the streams carrying the
    // realtime pipeline.
    sc.MaxBytes = 384 * 1024LL * 1024;
    // The poller deliberately re-reads a recent window on every pass, because
    // both intraday sources are archives written after the fact. Every message
    // carries a deterministic id, so a window wider than that lookback turns
    // those repeats into no-ops at the server instead of duplicates on the
    // wire.
    sc.Duplicates = 48 * kNsPerHour;

    jsErrCode jerr = static_cast<jsErrCode>(0);
    jsStreamInfo* si = nullptr;
    s = js_GetStreamInfo(&si, js, cfg.stream.c_str(), nullptr, &jerr);
    bool ok = false;
    if (s == NATS_OK && si != nullptr) {
        jsStreamInfo_Destroy(si);
        si = nullptr;
        s = js_UpdateStream(&si, js, &sc, nullptr, &jerr);
        ok = (s == NATS_OK);
        log::write(ok ? log::Level::info : log::Level::error,
                   ok ? "stream_updated" : "stream_update_failed",
                   {{"stream", cfg.stream}, {"error", natsStatus_GetText(s)},
                    {"jetstream_code", static_cast<int>(jerr)}});
    } else if (s == NATS_NOT_FOUND || jerr == JSStreamNotFoundErr) {
        s = js_AddStream(&si, js, &sc, nullptr, &jerr);
        ok = (s == NATS_OK);
        log::write(ok ? log::Level::info : log::Level::error,
                   ok ? "stream_created" : "stream_create_failed",
                   {{"stream", cfg.stream}, {"subjects", bars + "," + refs},
                    {"error", natsStatus_GetText(s)},
                    // Without this the server's reason -- insufficient storage,
                    // a subject overlap, a limit -- is invisible and the failure
                    // reads as a bare "Error".
                    {"jetstream_code", static_cast<int>(jerr)},
                    {"max_bytes", static_cast<int64_t>(sc.MaxBytes)}});
    } else {
        log::error("stream_info_failed",
                   {{"stream", cfg.stream}, {"error", natsStatus_GetText(s)}});
    }

    if (si != nullptr) jsStreamInfo_Destroy(si);
    jsCtx_Destroy(js);
    natsConnection_Destroy(conn);
    return ok;
}

} // namespace altmacro::messaging
