#pragma once

#include "core/error.hpp"

#include <pqxx/pqxx>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace altspotter::persistence {

// Bounded PostgreSQL connection pool. pqxx::connection is not thread-safe,
// so connections are leased exclusively. Broken connections are discarded
// and lazily re-established on the next lease.
class PgPool {
public:
    PgPool(std::string dsn, size_t size);

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
            return {};
        } catch (const pqxx::broken_connection& e) {
            lease->discard();
            return fail(ErrorCode::database_error, std::string("connection lost: ") + e.what(), true);
        } catch (const pqxx::transaction_rollback& e) {
            // Deadlock / serialization failure: transient by definition, retry.
            return fail(ErrorCode::database_error,
                        std::string("transaction rolled back: ") + e.what(), true);
        } catch (const pqxx::sql_error& e) {
            return fail(ErrorCode::database_error,
                        std::string("SQL error: ") + e.what() + " [" + e.query().substr(0, 200) + "]");
        } catch (const std::exception& e) {
            return fail(ErrorCode::database_error, std::string("database error: ") + e.what());
        }
    }

private:
    friend class Lease;
    void release(std::unique_ptr<pqxx::connection> conn);

    std::string dsn_;
    size_t size_;
    size_t created_ = 0;
    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<std::unique_ptr<pqxx::connection>> idle_;
};

// Quote a timestamptz literal from Unix milliseconds (ISO-8601 UTC).
std::string ts_literal(pqxx::transaction_base& txn, int64_t ms);

} // namespace altspotter::persistence
