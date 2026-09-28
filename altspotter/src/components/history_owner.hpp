#pragma once

#include "components/history_runs.hpp"
#include "core/error.hpp"
#include "logging/logger.hpp"

#include <pqxx/pqxx>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace altspotter::history {

// A run that a previous owner session left 'running' and the new owner closed.
struct OrphanedRun {
    int64_t id = 0;
    std::string kind;
    int64_t exchange_id = 0;
    int64_t instrument_id = 0;
    int64_t from_ms = 0;      // 0 when unknown
    int64_t to_ms = 0;        // 0 when unknown
    int64_t started_ms = 0;
    bool bootstrap_resumed = false;
};

// The owner session of market_history (history_runs.hpp): the one connection
// that holds the run-owner lock, and the only one run state is written on.
//
// Thread-safe. The bootstrap, repair and main threads share it; each call holds
// the session for one short statement or transaction.
//
// Ownership is lost for good: once the session is gone (a failed statement, a
// terminated backend, a PostgreSQL restart) lost() stays true and no call ever
// reconnects to take the lock again. The incarnation has to end and a fresh one
// compete for the lock -- otherwise a process could regain the lock after
// another owner had already closed its runs, and carry on as if nothing had
// happened.
class RunOwner {
public:
    explicit RunOwner(std::string dsn) : dsn_(std::move(dsn)) {}
    RunOwner(const RunOwner&) = delete;
    RunOwner& operator=(const RunOwner&) = delete;

    // One attempt to become the owner: open the session if there is none and
    // try the lock. True once this session holds it. An error means the
    // database could not be asked; false, that another session holds the lock.
    Result<bool> take();

    // Whether this session still holds the lock. A statement on the session
    // itself, so a session that has ended answers false. Marks the ownership
    // lost when it does not.
    bool holds();

    [[nodiscard]] bool owner() const { return owner_.load() && !lost_.load(); }
    [[nodiscard]] bool lost() const { return lost_.load(); }

    // Close the runs earlier owner sessions left 'running'. Only meaningful
    // straight after take() returned true, before this incarnation starts a run.
    Result<std::vector<OrphanedRun>> close_orphans();

    // Run `fn` in one transaction on the owner session. This is the fence: it
    // refuses to run unless this session is the owner, and a session that
    // ended cannot commit, so no process without the lock ever writes run
    // state. False when refused or failed (logged under `context`).
    template <typename Fn>
    bool with_txn(const char* context, Fn&& fn) {
        std::lock_guard lock(mu_);
        if (!owner_.load() || lost_.load() || !conn_) {
            log::warn("history_run_write_fenced",
                      {{"context", context}, {"message", "this session does not own the runs"}});
            return false;
        }
        try {
            pqxx::work txn(*conn_);
            fn(txn);
            txn.commit();
            return true;
        } catch (const pqxx::broken_connection& e) {
            mark_lost(context, e.what());
        } catch (const pqxx::in_doubt_error& e) {
            // The commit may or may not have landed; either way this session is
            // no longer usable, and the next owner settles the run.
            mark_lost(context, e.what());
        } catch (const std::exception& e) {
            log::error("db_txn_failed", {{"context", context}, {"message", e.what()}});
        }
        return false;
    }

private:
    void mark_lost(const char* context, const std::string& message);

    std::string dsn_;
    std::mutex mu_;
    std::unique_ptr<pqxx::connection> conn_;
    std::atomic<bool> owner_{false};
    std::atomic<bool> lost_{false};
};

} // namespace altspotter::history
