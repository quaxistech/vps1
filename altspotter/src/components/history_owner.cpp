#include "components/history_owner.hpp"

namespace altspotter::history {

Result<bool> RunOwner::take() {
    std::lock_guard lock(mu_);
    if (lost_.load()) return false;   // this incarnation is done with ownership
    if (owner_.load()) return true;
    try {
        if (!conn_) conn_ = std::make_unique<pqxx::connection>(dsn_);
        pqxx::nontransaction tx(*conn_);
        const bool taken =
            tx.exec_params(kTakeRunOwnershipSql, kRunOwnerLockSpace, kRunOwnerLockKey)[0][0]
                .as<bool>();
        if (taken) owner_.store(true);
        return taken;
    } catch (const std::exception& e) {
        conn_.reset();   // the next attempt opens a fresh session
        return fail(ErrorCode::database_error, std::string("run ownership: ") + e.what(), true);
    }
}

bool RunOwner::holds() {
    std::lock_guard lock(mu_);
    if (!owner_.load() || lost_.load() || !conn_) return false;
    try {
        pqxx::nontransaction tx(*conn_);
        if (tx.exec_params(kHoldsRunOwnershipSql, kRunOwnerLockSpace, kRunOwnerLockKey)[0][0]
                .as<bool>()) {
            return true;
        }
        mark_lost("holds", "the owner session no longer holds the lock");
    } catch (const std::exception& e) {
        mark_lost("holds", e.what());
    }
    return false;
}

Result<std::vector<OrphanedRun>> RunOwner::close_orphans() {
    std::vector<OrphanedRun> closed;
    const bool committed = with_txn("history_orphan_recovery", [&](pqxx::work& txn) {
        closed.clear();
        for (const auto& r : txn.exec(kCloseOrphanedRunsSql)) {
            OrphanedRun o;
            o.id = r[0].as<int64_t>();
            o.kind = r[1].as<std::string>();
            o.exchange_id = r[2].as<int64_t>();
            o.instrument_id = r[3].as<int64_t>();
            o.from_ms = r[4].as<int64_t>(0);
            o.to_ms = r[5].as<int64_t>(0);
            o.started_ms = r[6].as<int64_t>(0);
            o.bootstrap_resumed = r[7].as<bool>(false);
            closed.push_back(std::move(o));
        }
    });
    if (!committed) {
        return fail(ErrorCode::database_error, "orphaned runs were not closed", true);
    }
    return closed;
}

// Called with mu_ held. Dropping the connection makes sure the session is
// really gone, so a lock it might still hold on the server is released too.
void RunOwner::mark_lost(const char* context, const std::string& message) {
    if (!lost_.exchange(true)) {
        log::error("history_run_ownership_lost",
                   {{"context", context},
                    {"message", message},
                    {"action", "run state is fenced; work stops and the component restarts"}});
    }
    conn_.reset();
}

} // namespace altspotter::history
