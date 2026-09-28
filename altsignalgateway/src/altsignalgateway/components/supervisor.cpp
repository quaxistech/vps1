#include "altsignalgateway/components/supervisor.hpp"

#include "altsignalgateway/components/delivery_pump.hpp"
#include "altsignalgateway/components/query_service.hpp"
#include "altsignalgateway/components/ingestor.hpp"
#include "altsignalgateway/components/session_registry.hpp"
#include "altsignalgateway/components/ws_server.hpp"
#include "altsignalgateway/core/logger.hpp"
#include "altsignalgateway/core/nats_provision.hpp"
#include "altsignalgateway/core/readiness.hpp"
#include "altsignalgateway/core/shutdown.hpp"
#include "altsignalgateway/core/time.hpp"
#include "altsignalgateway/data/store.hpp"

#include <memory>

namespace altsignalgateway::components {

namespace {
// How often the supervisor wakes to service the watchdog and refresh the
// systemd status line. It must be well under the shortest of those two budgets
// (the watchdog ping interval, which is half of WatchdogSec) so a single missed
// wakeup can never look like a wedged process.
constexpr auto kSupervisorTick = std::chrono::milliseconds(500);
// How often the one-line systemd status is refreshed. It is read by a human
// running `systemctl status`, so it is paced for legibility rather than for
// resolution: more often would mean a database round trip per second for a line
// nobody is watching that closely.
constexpr int64_t kStatusIntervalMs = 15'000;
// How often the single-writer advisory lock is re-verified. The lock dies with
// its connection, so a database restart silently removes it; this is the
// interval within which that is noticed. Short enough that the window in which
// two writers could overlap is seconds, cheap enough to be a bare `SELECT 1`.
constexpr int64_t kWriterLockCheckMs = 5'000;
// How long a worker thread may go without completing a loop iteration before it
// is treated as WEDGED. The delivery pump wakes at least every 250 ms and the
// ingest loop at least every fetch timeout plus its longest backoff, so these
// are two orders of magnitude above a healthy cycle: they fire for a deadlock,
// never for a slow dependency.
constexpr int64_t kPumpStaleMs = 30'000;
constexpr int64_t kIngestStaleMs = 120'000;
} // namespace

Result<int> run_service(const config::RuntimeConfig& cfg, const std::string& version) {
    auto& shutdown = ShutdownToken::instance();

    // --- credentials ---------------------------------------------------------
    auto raw = config::load_credentials(cfg.auth.credentials_file);
    if (!raw) return std::unexpected(raw.error());
    std::vector<domain::RawCredential> creds;
    creds.reserve(raw->size());
    for (const auto& c : *raw) {
        creds.push_back(domain::RawCredential{c.client_id, c.key_id, c.secret_hex, c.capabilities,
                                              c.max_sessions});
    }
    auto clients = domain::ClientRegistry::build(creds);
    if (!clients) return std::unexpected(clients.error());
    // The secrets are now only in the registry. Wipe the hex copies rather than
    // leaving them in a vector that lives until the end of the function.
    for (auto& c : creds) c.secret_hex.assign(c.secret_hex.size(), '0');
    for (auto& c : *raw) c.secret_hex.assign(c.secret_hex.size(), '0');
    log::info("credentials_loaded", {{"clients", clients->size()},
                                     {"file", cfg.auth.credentials_file}});

    // --- journal -------------------------------------------------------------
    auto store = std::make_shared<data::JournalStore>(cfg, cfg.common.instance_id);
    if (auto r = store->bootstrap(); !r) return std::unexpected(r.error());
    auto reclaimed = store->recover_in_flight();
    if (!reclaimed) return std::unexpected(reclaimed.error());
    if (auto r = store->reconcile_retention_policies(); !r) return std::unexpected(r.error());
    auto stats = store->stats();
    if (!stats) return std::unexpected(stats.error());
    if (auto r = store->note_started(timeutil::now_ms()); !r) {
        log::warn("service_state_update_failed", {{"message", r.error().message}});
    }
    log::info("journal_recovered", {{"reclaimed_in_flight", *reclaimed},
                                    {"head_sequence", stats->head_sequence},
                                    {"rows", stats->total_rows},
                                    {"undelivered", stats->journaled},
                                    {"acked", stats->acked},
                                    {"expired", stats->expired},
                                    {"failed", stats->failed}});

    // --- JetStream topology ---------------------------------------------------
    // Run inline at every start so a cold boot cannot race JetStream becoming
    // available. Idempotent; creates only this service's own durable.
    if (!messaging::nats_provision(cfg.common.nats_url, cfg.wiring,
                                   cfg.wiring.final_deliver_policy == "all")) {
        return fail(ErrorCode::messaging_error,
                    "JetStream topology could not be validated or provisioned");
    }

    // --- delivery + listener --------------------------------------------------
    SessionRegistry sessions(cfg.websocket.max_clients, cfg.auth.auth_failure_threshold,
                             cfg.auth.auth_failure_window_sec, cfg.auth.auth_lockout_sec);
    DeliveryPump pump(cfg, store, sessions, version);
    pump.start();

    // Queries run on their own thread so a slow foreign read can never sit in
    // front of a trade delivery.
    QueryService queries(store, cfg, version);
    queries.start();

    WsServer server(cfg, *clients, sessions, pump, queries, version);
    if (auto r = server.prepare(); !r) {
        queries.stop();
        pump.stop();
        return std::unexpected(r.error());
    }
    server.start();

    // --- ingest ---------------------------------------------------------------
    Ingestor ingest(cfg, store, version);
    ingest.on_journalled([&pump] { pump.wake(); });
    ingest.start();

    notify_ready();
    log::info("service_ready", {{"version", version},
                                {"instance", cfg.common.instance_id},
                                {"wiring", cfg.wiring.id},
                                {"input_stream", cfg.wiring.final_stream},
                                {"durable", cfg.wiring.final_durable},
                                {"execution_exchange", cfg.wiring.execution_exchange},
                                {"listen", cfg.websocket.listen_address + ":" +
                                               std::to_string(server.bound_port())},
                                {"journal_head", stats->head_sequence}});

    const int64_t watchdog_ms = watchdog_interval_ms();
    int64_t next_watchdog = timeutil::now_ms();
    int64_t next_status = 0;
    int64_t next_lock_check = timeutil::now_ms() + kWriterLockCheckMs;
    bool writer_lock_held = true;

    while (!shutdown.stop_requested()) {
        shutdown.wait_for(kSupervisorTick);
        const int64_t now = timeutil::now_ms();

        if (now >= next_lock_check) {
            next_lock_check = now + kWriterLockCheckMs;
            if (auto held = store->verify_writer_lock(); !held) {
                writer_lock_held = false;
                if (held.error().retryable) {
                    // The database is unreachable. The lock cannot be confirmed
                    // OR denied, and neither can anything else be written, so
                    // this is the same condition the pump already reports: stay
                    // up, stop claiming health, and keep trying.
                    log::error("writer_lock_unverifiable",
                               {{"message", held.error().message}});
                } else {
                    // Another instance owns the journal. Continuing would mean
                    // two writers sharing one sequence and two independent
                    // views of every client's position.
                    log::critical("writer_lock_lost", {{"message", held.error().message}});
                    shutdown.request_stop();
                    break;
                }
            } else {
                writer_lock_held = true;
            }
        }

        const int64_t pump_stale = now - pump.last_tick_ms();
        const int64_t ingest_stale = now - ingest.last_tick_ms();
        if (ingest_stale > kIngestStaleMs) {
            log::warn("ingest_thread_stalled", {{"silent_ms", ingest_stale}});
        }

        if (watchdog_ms > 0 && now >= next_watchdog) {
            // The watchdog answers ONE question: is this process still doing
            // its work, or is it wedged? A restart repairs a wedged thread. It
            // does not repair an unreachable database or a stopped broker — and
            // withholding the ping for those is how a dependency outage turns
            // into a killed process and, once the restart budget is spent, into
            // an outage of the boundary itself that outlives the cause.
            //
            // That is not hypothetical: withholding the ping on `pump.healthy()`
            // had systemd SIGABRT this service during a PostgreSQL restart, and
            // the journal shows `Failed with result 'watchdog'`. Dependency
            // health is reported in the status line and the logs instead.
            const bool wedged = pump_stale > kPumpStaleMs;
            if (!wedged && server.listening()) {
                notify_watchdog();
            } else {
                log::error("watchdog_ping_withheld",
                           {{"pump_stale_ms", pump_stale},
                            {"listening", server.listening()},
                            {"reason", wedged ? "the delivery pump is not advancing"
                                              : "the listener is down"}});
            }
            next_watchdog = now + watchdog_ms;
        }

        if (now >= next_status) {
            next_status = now + kStatusIntervalMs;
            auto s = store->stats();
            const std::string line =
                "clients=" + std::to_string(sessions.authenticated_count()) + "/" +
                std::to_string(sessions.size()) +
                " head=" + std::to_string(s ? s->head_sequence : -1) +
                " backlog=" + std::to_string(s ? s->journaled + s->queued + s->sent : -1) +
                " journalled=" + std::to_string(ingest.journalled()) +
                " nats=" + (ingest.connected() ? "up" : "down") +
                " db=" + (pump.healthy() ? "up" : "down") +
                (writer_lock_held ? "" : " writer_lock=UNVERIFIED") +
                (ingest_stale > kIngestStaleMs ? " ingest=STALLED" : "");
            set_component_status("gateway", line);
        }
    }

    // --- graceful shutdown ----------------------------------------------------
    notify_stopping();
    const auto stop_started = std::chrono::steady_clock::now();
    const auto stop_budget = std::chrono::seconds(cfg.common.shutdown_timeout_sec);
    log::info("shutdown_requested", {{"budget_sec", cfg.common.shutdown_timeout_sec}});
    // Ingest first: stop taking on new obligations before giving up the ability
    // to meet them.
    ingest.stop();
    // Then the listener, which requeues everything its sessions were holding.
    server.stop();
    queries.stop();
    // The pump last, so those requeues are actually written.
    pump.stop();
    const auto stop_took =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - stop_started);
    if (stop_took > stop_budget) {
        // Not fatal — everything above has already completed by the time this
        // runs — but a stop that outgrows its budget is on its way to being
        // SIGKILLed by systemd's TimeoutStopSec, and the operator should learn
        // that from a log line rather than from a truncated shutdown.
        log::warn("shutdown_exceeded_budget", {{"took_ms", stop_took.count()},
                                               {"budget_sec", cfg.common.shutdown_timeout_sec}});
    }

    auto final_stats = store->stats();
    log::info("shutdown_complete",
              {{"journalled", ingest.journalled()},
               {"duplicates", ingest.duplicates()},
               {"rejected", ingest.rejected()},
               {"integrity_conflicts", ingest.conflicts()},
               {"head_sequence", final_stats ? final_stats->head_sequence : -1},
               {"undelivered", final_stats ? final_stats->journaled : -1}});
    return 0;
}

} // namespace altsignalgateway::components
