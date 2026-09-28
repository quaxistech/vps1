#pragma once
// Internal AltTech PostgreSQL adapter API.

#include "alttech/common/error.hpp"

#include <pqxx/pqxx>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace alttech::persistence {

// Bounded PostgreSQL connection pool. pqxx::connection is not thread-safe,
// so connections are leased exclusively. Broken connections are discarded
// and lazily re-established on the next lease.
class PgPool {
public:
    // `statement_timeout_ms` bounds every server-side query on every pooled
    // connection so a stuck query (e.g. a wedged backend or a lock wait) turns
    // into a retryable error instead of hanging a worker thread indefinitely.
    // 0 disables the bound. Applied once per physical connection at creation.
    PgPool(std::string dsn, size_t size, int64_t statement_timeout_ms = 120'000);

    class Lease {
    public:
        Lease(PgPool& pool, std::unique_ptr<pqxx::connection> conn)
            : pool_(&pool), conn_(std::move(conn)) {}
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& o) noexcept : pool_(o.pool_), conn_(std::move(o.conn_)) { o.pool_ = nullptr; }
        ~Lease();

        pqxx::connection& conn() { return *conn_; }
        // Mark connection as broken; it will not be returned to the pool.
        void discard() { conn_.reset(); }

    private:
        PgPool* pool_;
        std::unique_ptr<pqxx::connection> conn_;
    };

    // Blocks until a connection is available or timeout; creates lazily up to `size`.
    Result<Lease> acquire(std::chrono::milliseconds timeout = std::chrono::seconds(30));

    // How many transactions in a row have failed because the database no longer
    // matches this binary. Reset by any successful transaction, so a single odd
    // query cannot trip it but a vanished schema will. The process health loop
    // reads this: a service that cannot reach its own schema must stop, not
    // retry forever while still reporting itself healthy.
    [[nodiscard]] int consecutive_schema_errors() const noexcept {
        return consecutive_schema_errors_.load(std::memory_order_relaxed);
    }

    // Convenience: run `fn(pqxx::work&)` in a transaction with commit.
    // On connection failure the connection is discarded and the error returned.
    template <typename Fn>
    Result<void> with_txn(Fn&& fn) {
        auto lease = acquire();
        if (!lease) return std::unexpected(lease.error());
        try {
            pqxx::work txn(lease->conn());
            fn(txn);
            txn.commit();
            consecutive_schema_errors_.store(0, std::memory_order_relaxed);
            return {};
        } catch (const pqxx::broken_connection& e) {
            lease->discard();
            return fail(ErrorCode::database_error, std::string("connection lost: ") + e.what(), true);
        } catch (const pqxx::sql_error& e) {
            // PostgreSQL says which kind of failure this is; believe it. A
            // missing schema/table or a revoked privilege can never be fixed by
            // retrying, so it must not be reported as a retryable database
            // error — that is how a service ends up looping for hours while
            // systemd still believes it is healthy.
            const std::string state = e.sqlstate();
            const bool schema_gone =
                state == "42P01" ||  // undefined_table
                state == "42P02" ||  // undefined_parameter
                state == "42703" ||  // undefined_column
                state == "42883" ||  // undefined_function
                state == "3F000" ||  // invalid_schema_name
                state == "42501";    // insufficient_privilege
            if (schema_gone) {
                consecutive_schema_errors_.fetch_add(1, std::memory_order_relaxed);
            }
            // The opposite case: another transaction held a lock, so exactly the
            // same statement will succeed once it commits. Marking it retryable
            // keeps it from being read as a fault in the data or the schema.
            const bool contended = state == "40P01" ||  // deadlock_detected
                                   state == "40001" ||  // serialization_failure
                                   state == "55P03";    // lock_not_available
            return fail(schema_gone ? ErrorCode::schema_error : ErrorCode::database_error,
                        std::string("SQL error [") + state + "]: " + e.what() + " [" +
                            e.query().substr(0, 200) + "]",
                        contended);
        } catch (const std::exception& e) {
            return fail(ErrorCode::database_error, std::string("database error: ") + e.what());
        }
    }

private:
    friend class Lease;
    void release(std::unique_ptr<pqxx::connection> conn);

    std::string dsn_;
    size_t size_;
    int64_t statement_timeout_ms_ = 120'000;
    size_t created_ = 0;
    std::atomic<int> consecutive_schema_errors_{0};
    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<std::unique_ptr<pqxx::connection>> idle_;
};

// Quote a timestamptz literal from Unix milliseconds (ISO-8601 UTC).
std::string ts_literal(pqxx::transaction_base& txn, int64_t ms);

// How many consecutive schema failures mean the database really is gone rather
// than one query being odd. Small enough to react within seconds, large enough
// that a single unlucky statement cannot stop the process.
inline constexpr int kSchemaFailureThreshold = 5;

// Whether the process should stop because the database no longer matches it.
[[nodiscard]] constexpr bool schema_integrity_lost(int consecutive_schema_errors) noexcept {
    return consecutive_schema_errors >= kSchemaFailureThreshold;
}

} // namespace alttech::persistence
