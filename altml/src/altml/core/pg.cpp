#include "altml/core/pg.hpp"

#include "altml/core/shutdown.hpp"
#include "altml/core/time.hpp"

namespace altml::persistence {

namespace {
std::mutex& registry_mu() { static std::mutex m; return m; }
std::set<PgPool*>& registry() { static std::set<PgPool*> r; return r; }
} // namespace

PgPool::PgPool(std::string dsn, size_t size) : dsn_(std::move(dsn)), size_(size == 0 ? 1 : size) {
    std::lock_guard lock(registry_mu());
    registry().insert(this);
}

PgPool::~PgPool() {
    std::lock_guard lock(registry_mu());
    registry().erase(this);
}

void PgPool::track(pqxx::connection* c, bool leased) {
    std::lock_guard lock(leased_mu_);
    if (leased) leased_.insert(c); else leased_.erase(c);
}

void PgPool::cancel_all_in_flight() {
    std::lock_guard lock(registry_mu());
    for (PgPool* p : registry()) {
        std::lock_guard l2(p->leased_mu_);
        for (pqxx::connection* c : p->leased_) {
            try { c->cancel_query(); } catch (...) {}
        }
    }
}

PgPool::Lease::~Lease() {
    if (pool_ != nullptr && conn_ != nullptr) {
        pool_->track(conn_.get(), false);
        pool_->release(std::move(conn_));
    } else if (pool_ != nullptr) {
        // Discarded (broken) connection frees a creation slot.
        std::lock_guard lock(pool_->mu_);
        --pool_->created_;
        pool_->cv_.notify_one();
    }
}

Result<PgPool::Lease> PgPool::acquire(std::chrono::milliseconds timeout) {
    // Stopping: no new statement starts (cancel_all_in_flight() stopped the
    // running ones), so every component unwinds to its stop check.
    if (ShutdownToken::instance().stop_requested()) {
        return fail(ErrorCode::shutdown_requested, "shutting down", true);
    }
    std::unique_lock lock(mu_);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (!idle_.empty()) {
            auto conn = std::move(idle_.back());
            idle_.pop_back();
            if (conn->is_open()) return Lease{*this, std::move(conn)};
            --created_; // stale connection dropped
            continue;
        }
        if (created_ < size_) {
            ++created_;
            lock.unlock();
            try {
                auto conn = std::make_unique<pqxx::connection>(dsn_);
                return Lease{*this, std::move(conn)};
            } catch (const std::exception& e) {
                lock.lock();
                --created_;
                cv_.notify_one();
                return fail(ErrorCode::database_error,
                            std::string("failed to connect to PostgreSQL: ") + e.what(), true);
            }
        }
        if (cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
            return fail(ErrorCode::database_error, "timed out waiting for a pooled connection", true);
        }
    }
}

void PgPool::release(std::unique_ptr<pqxx::connection> conn) {
    std::lock_guard lock(mu_);
    if (conn->is_open()) {
        idle_.push_back(std::move(conn));
    } else {
        --created_;
    }
    cv_.notify_one();
}

std::string ts_literal(pqxx::transaction_base& txn, int64_t ms) {
    return txn.quote(timeutil::to_iso8601(ms)) + "::timestamptz";
}

} // namespace altml::persistence
