// AltTech PostgreSQL adapter implementation.
#include "alttech/persistence/pg.hpp"

#include "alttech/time/time.hpp"

namespace alttech::persistence {

PgPool::PgPool(std::string dsn, size_t size, int64_t statement_timeout_ms)
    : dsn_(std::move(dsn)),
      size_(size == 0 ? 1 : size),
      statement_timeout_ms_(statement_timeout_ms < 0 ? 0 : statement_timeout_ms) {}

PgPool::Lease::~Lease() {
    if (pool_ != nullptr && conn_ != nullptr) {
        pool_->release(std::move(conn_));
    } else if (pool_ != nullptr) {
        // Discarded (broken) connection frees a creation slot.
        std::lock_guard lock(pool_->mu_);
        --pool_->created_;
        pool_->cv_.notify_one();
    }
}

Result<PgPool::Lease> PgPool::acquire(std::chrono::milliseconds timeout) {
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
                if (statement_timeout_ms_ > 0) {
                    // Session-level bound so no server-side query can block a
                    // worker forever; a breached bound surfaces as a retryable
                    // pqxx::sql_error, not a hung thread.
                    pqxx::nontransaction setup(*conn);
                    setup.exec("SET statement_timeout = " +
                               std::to_string(statement_timeout_ms_));
                }
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

} // namespace alttech::persistence
